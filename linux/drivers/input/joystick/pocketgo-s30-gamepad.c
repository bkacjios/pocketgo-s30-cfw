// SPDX-License-Identifier: GPL-2.0-only
/*
 * PocketGo S30 gamepad: the analog stick, read through a microcontroller on
 * I2C, and the gamepad buttons on GPIOs, reported as one input device so
 * that emulators see a single controller.
 *
 * Stick protocol (from the stock "mcu_ts" driver): a plain 4 byte read
 * returns A, ~A, B, ~B; samples where a byte and its complement do not add
 * up to 0xff are torn and dropped. Further bytes only repeat the last one.
 * As mounted, A is the vertical axis (low = up) and B the horizontal one
 * (high = right), matching ABS_Y and ABS_X directly.
 *
 * Stick calibration (sysfs "calibration", written by the calibration in
 * s30-display): "cx cy m00 m01 m10 m11 [deadzone [e0 .. e31]]", v = M *
 * (raw - c) per sample, M in Q12 fixed point (4096 = 1.0). Without the edge
 * table v is reported as is; with it, e0..e31 are how far v reaches (in 1/16
 * raw counts) in 32 directions, from +x (right) towards +y (down), and v is
 * scaled by 127 over the edge in its direction (interpolated between the two
 * nearest), so a stick of any shape reaches full deflection all round.
 * Reported = 128 + v, clamped to 0-255. The default (128 128 4096 0 0 4096 8)
 * passes raw values through apart from the deadzone; the calibration sets
 * deadzone 0 and no edges to read raw values.
 *
 * The stick is read by its own thread, which keeps the latest sample; the
 * poll reports that with the buttons and never waits on I2C. The stick MCU
 * sometimes holds the bus (seconds, with the controller's default timeout),
 * which used to stop the buttons with it. The bus carries only the stick, so
 * its timeout is cut to S30_I2C_TIMEOUT_MS.
 *
 * Before calibration the raw samples are smoothed (each output the average of
 * the new sample and the previous output, about 20 ms of lag at the 10 ms
 * poll); after it, a radial deadzone (out of 127, default S30_DEADZONE) around the
 * centre gives a steady zero at rest, rescaling the rest of the range so the
 * edge still reaches full deflection.
 */

#include <linux/bits.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/math.h>
#include <linux/mutex.h>
#include <linux/property.h>

#define S30_GAMEPAD_POLL_MS	10
#define S30_I2C_TIMEOUT_MS	50
#define S30_DEADZONE		8
#define S30_GAMEPAD_MAX_KEYS	32
#define S30_EDGES		32	/* directions in the edge table */

struct s30_gamepad_key {
	struct gpio_desc *gpiod;
	unsigned int code;
};

struct s30_gamepad {
	struct i2c_client *client;
	struct input_dev *input;
	struct mutex cal_lock;
	int cal[7];		/* cx cy m00 m01 m10 m11 deadzone */
	int edge[S30_EDGES];	/* reach per direction, 1/16 counts (0: no table) */
	int fx, fy;		/* smoothed raw stick, 1/16 units (-1: none yet) */
	int stick;		/* latest reported stick, x | y << 8 (-1: none yet) */
	unsigned int interval;	/* ms between stick reads */
	struct task_struct *thread;
	unsigned int nkeys;
	struct s30_gamepad_key keys[] __counted_by(nkeys);
};

/*
 * Direction of (x, y) in 1/65536 turns from +x towards +y: per octant,
 * atan(z) / 2pi ~ z / 8 + 0.0435 z (1 - z) for z = min / max (error about
 * 0.0006 turn, 1/50 of a table step)
 */
static u32 s30_turn(s64 x, s64 y)
{
	s64 ax = abs(x), ay = abs(y), lo = min(ax, ay), hi = max(ax, ay);
	s64 z, a;

	if (hi == 0)
		return 0;
	z = div64_s64(lo << 16, hi);		/* 0..65536 */
	a = z / 8 + div64_s64(2848 * z * (65536 - z), 65536LL * 65536);
	if (ay > ax)
		a = 16384 - a;			/* from the y axis */
	if (x < 0)
		a = 32768 - a;
	if (y < 0)
		a = 65536 - a;
	return (u32)a & 0xffff;
}

/* The edge table's reach in the direction of (x, y), 1/16 counts */
static int s30_edge(const int *edge, int x, int y)
{
	u32 pos = s30_turn(x, y) * S30_EDGES;	/* Q16 table index */
	u32 i = pos >> 16, f = pos & 0xffff;

	return (edge[i] * (65536 - f) + edge[(i + 1) % S30_EDGES] * f) >> 16;
}

/* One stick sample, calibrated, into gp->stick */
static void s30_stick_read(struct s30_gamepad *gp)
{
	u8 buf[4];
	int ret;

	ret = i2c_master_recv(gp->client, buf, sizeof(buf));
	if (ret == sizeof(buf) && (u8)(buf[0] + buf[1]) == 0xff &&
	    (u8)(buf[2] + buf[3]) == 0xff) {
		int dx, dy, vx, vy, x, y, r, dz;

		if (gp->fx < 0) {
			gp->fx = buf[2] * 16;
			gp->fy = buf[0] * 16;
		} else {
			gp->fx = (gp->fx + buf[2] * 16) / 2;
			gp->fy = (gp->fy + buf[0] * 16) / 2;
		}
		mutex_lock(&gp->cal_lock);
		dx = gp->fx - gp->cal[0] * 16;
		dy = gp->fy - gp->cal[1] * 16;
		vx = (gp->cal[2] * dx + gp->cal[3] * dy) / 4096; /* 1/16 counts */
		vy = (gp->cal[4] * dx + gp->cal[5] * dy) / 4096;
		if (gp->edge[0]) {
			int e = s30_edge(gp->edge, vx, vy);

			/* out to the clamp below, beyond the edge */
			x = clamp(vx * 127 / e, -255, 255);
			y = clamp(vy * 127 / e, -255, 255);
		} else {
			x = vx / 16;
			y = vy / 16;
		}
		dz = gp->cal[6];
		mutex_unlock(&gp->cal_lock);

		r = int_sqrt(x * x + y * y);
		if (dz == 0) {
			/* raw: no deadzone */
		} else if (r <= dz) {
			x = 0;
			y = 0;
		} else {
			int scaled = (r - dz) * 127 / (127 - dz);

			x = x * scaled / r;
			y = y * scaled / r;
		}
		WRITE_ONCE(gp->stick, clamp(128 + x, 0, 255) | clamp(128 + y, 0, 255) << 8);
	}
}

static int s30_stick_thread(void *data)
{
	struct s30_gamepad *gp = data;

	while (!kthread_should_stop()) {
		s30_stick_read(gp);
		usleep_range(gp->interval * 1000, gp->interval * 1000 + 1000);
	}
	return 0;
}

static int s30_gamepad_open(struct input_dev *input)
{
	struct s30_gamepad *gp = input_get_drvdata(input);

	gp->thread = kthread_run(s30_stick_thread, gp, "s30-stick");
	return PTR_ERR_OR_ZERO(gp->thread);
}

static void s30_gamepad_close(struct input_dev *input)
{
	struct s30_gamepad *gp = input_get_drvdata(input);

	kthread_stop(gp->thread);
}

/* The latest stick sample and the buttons: nothing here waits on I2C */
static void s30_gamepad_poll(struct input_dev *input)
{
	struct s30_gamepad *gp = input_get_drvdata(input);
	int stick = READ_ONCE(gp->stick), ret;
	unsigned int i;

	if (stick >= 0) {
		input_report_abs(input, ABS_X, stick & 0xff);
		input_report_abs(input, ABS_Y, stick >> 8);
	}

	for (i = 0; i < gp->nkeys; i++) {
		ret = gpiod_get_value_cansleep(gp->keys[i].gpiod);
		if (ret >= 0)
			input_report_key(input, gp->keys[i].code, ret);
	}

	input_sync(input);
}

static ssize_t calibration_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct s30_gamepad *gp = i2c_get_clientdata(to_i2c_client(dev));
	ssize_t len;
	int i;

	mutex_lock(&gp->cal_lock);
	len = sysfs_emit(buf, "%d %d %d %d %d %d %d", gp->cal[0], gp->cal[1],
			 gp->cal[2], gp->cal[3], gp->cal[4], gp->cal[5],
			 gp->cal[6]);
	for (i = 0; gp->edge[0] && i < S30_EDGES; i++)
		len += sysfs_emit_at(buf, len, " %d", gp->edge[i]);
	len += sysfs_emit_at(buf, len, "\n");
	mutex_unlock(&gp->cal_lock);
	return len;
}

static ssize_t calibration_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct s30_gamepad *gp = i2c_get_clientdata(to_i2c_client(dev));
	int c[7 + S30_EDGES] = { 0 }, i, n, used;
	const char *p = buf;

	for (n = 0; n < (int)ARRAY_SIZE(c) && sscanf(p, "%d%n", &c[n], &used) == 1; n++)
		p += used;
	if (n == 6)
		c[6] = S30_DEADZONE;
	else if ((n != 7 && n != (int)ARRAY_SIZE(c)) || c[6] < 0 || c[6] > 64)
		return -EINVAL;
	/* edges between 1 and 256 counts */
	for (i = 7; i < n; i++)
		if (c[i] < 16 || c[i] > 256 * 16)
			return -EINVAL;
	/* centre within the raw range, matrix entries within +-16.0 */
	if (c[0] < 0 || c[0] > 255 || c[1] < 0 || c[1] > 255)
		return -EINVAL;
	for (i = 2; i < 6; i++)
		if (abs(c[i]) > 16 * 4096)
			return -EINVAL;

	mutex_lock(&gp->cal_lock);
	memcpy(gp->cal, c, sizeof(gp->cal));
	memcpy(gp->edge, &c[7], sizeof(gp->edge)); /* zeros: no table */
	mutex_unlock(&gp->cal_lock);
	return count;
}
static DEVICE_ATTR_RW(calibration);

static struct attribute *s30_gamepad_attrs[] = {
	&dev_attr_calibration.attr,
	NULL
};
ATTRIBUTE_GROUPS(s30_gamepad);

static int s30_gamepad_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct input_dev *input;
	struct s30_gamepad *gp;
	unsigned int nkeys, i = 0;
	u32 interval;
	int ret;

	nkeys = device_get_child_node_count(dev);
	if (nkeys > S30_GAMEPAD_MAX_KEYS)
		return dev_err_probe(dev, -EINVAL, "too many buttons\n");

	gp = devm_kzalloc(dev, struct_size(gp, keys, nkeys), GFP_KERNEL);
	if (!gp)
		return -ENOMEM;
	gp->nkeys = nkeys;
	gp->client = client;
	mutex_init(&gp->cal_lock);
	gp->fx = -1;
	gp->stick = -1;
	gp->cal[0] = 128;
	gp->cal[1] = 128;
	gp->cal[2] = 4096;
	gp->cal[5] = 4096;
	gp->cal[6] = S30_DEADZONE;
	i2c_set_clientdata(client, gp);

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	gp->input = input;
	input->name = "PocketGo S30 Gamepad";
	input->phys = "pocketgo-s30-gamepad/input0";
	input->id.bustype = BUS_I2C;
	input_set_drvdata(input, gp);
	input->open = s30_gamepad_open;
	input->close = s30_gamepad_close;

	/* No fuzz/flat: the stick thread smooths and applies the deadzone, and
	 * the input core's fuzz filter turned slow movement into jumps */
	input_set_abs_params(input, ABS_X, 0, 255, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, 255, 0, 0);

	device_for_each_child_node_scoped(dev, child) {
		struct s30_gamepad_key *key = &gp->keys[i++];
		const char *label = NULL;

		fwnode_property_read_string(child, "label", &label);
		ret = fwnode_property_read_u32(child, "linux,code", &key->code);
		if (ret)
			return dev_err_probe(dev, ret, "%pfwP: no linux,code\n",
					     child);

		key->gpiod = devm_fwnode_gpiod_get(dev, child, NULL, GPIOD_IN,
						   label);
		if (IS_ERR(key->gpiod))
			return dev_err_probe(dev, PTR_ERR(key->gpiod),
					     "%pfwP: no gpio\n", child);

		input_set_capability(input, EV_KEY, key->code);
	}

	ret = input_setup_polling(input, s30_gamepad_poll);
	if (ret)
		return ret;

	if (device_property_read_u32(dev, "poll-interval", &interval))
		interval = S30_GAMEPAD_POLL_MS;
	input_set_poll_interval(input, interval);
	gp->interval = interval;

	/* The bus is the stick MCU's alone */
	client->adapter->timeout = msecs_to_jiffies(S30_I2C_TIMEOUT_MS);

	return input_register_device(input);
}

static const struct of_device_id s30_gamepad_of_match[] = {
	{ .compatible = "pocketgo,s30-gamepad" },
	{ }
};
MODULE_DEVICE_TABLE(of, s30_gamepad_of_match);

static const struct i2c_device_id s30_gamepad_id[] = {
	{ "s30-gamepad" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, s30_gamepad_id);

static struct i2c_driver s30_gamepad_driver = {
	.driver = {
		.name = "pocketgo-s30-gamepad",
		.of_match_table = s30_gamepad_of_match,
		.dev_groups = s30_gamepad_groups,
	},
	.probe = s30_gamepad_probe,
	.id_table = s30_gamepad_id,
};
module_i2c_driver(s30_gamepad_driver);

MODULE_DESCRIPTION("PocketGo S30 gamepad (I2C analog stick and GPIO buttons)");
MODULE_LICENSE("GPL");
