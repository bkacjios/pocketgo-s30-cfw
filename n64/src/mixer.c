/* The N64 audio microcode in software (s30_mixer.h): each game's Acmd macros
   run these directly. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "s30_mixer.h"

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;
typedef S30MixState ADPCM_STATE;
typedef S30MixState RESAMPLE_STATE;
#define A_INIT 0x01 // the decoder flags (abi.h)
#define A_LOOP 0x02

#define ROUND_UP_64(v) (((v) + 63) & ~63)
#define ROUND_UP_32(v) (((v) + 31) & ~31)
#define ROUND_UP_16(v) (((v) + 15) & ~15)
#define ROUND_UP_8(v)  (((v) + 7) & ~7)
#define ROUND_DOWN_16(v) ((v) & ~0xf)

/*
 * DMEM buffer: 8KB starting at address 0.
 * OoT uses DMEM addresses from 0x3C0 (DMEM_TEMP) up to ~0xE20 + data.
 */
#define DMEM_BUF_SIZE 0x1000
#define BUF_U8(a)  (rspa.buf + (a))
#define BUF_S16(a) ((s16*)(rspa.buf + (a)))
#define BUF_S32(a) ((s16*)(rspa.buf + (a)))

/* ---- Static mixer state (simulated RSP DMEM + registers) ---- */

static struct {
    u8 buf[DMEM_BUF_SIZE];

    u16 in;
    u16 out;
    u16 nbytes;

    u16 vol[2];       /* current vol L, R */
    u16 rate[2];      /* ramp rate L, R */
    u16 vol_wet;      /* reverb volume */
    u16 rate_wet;     /* reverb volume ramp */

    s16 adpcm_loop_state[16]; /* copied locally */
    float adpcm_table[16][2][8];  /* pre-divided by 2048 */

    u16 filter_count;
    s16 filter[8];

    s16 adpcm_raw[16 * 2 * 8]; /* the book as loaded (ABI1's pole filter reads it) */

    /* ABI1 (libultra 2.0's aspMain): the aux outputs, and the volumes */
    u16 dry_right, wet_left, wet_right;
    s16 dry, wet;
    s16 env_vol[2], env_target[2];
    s32 env_rate[2];
} __attribute__((aligned(32))) rspa;

/* ---- Float resample table (raw int16 values cast to float) ---- */
static float resample_table_f[64][4] = {
    { 3129, 26285, 3398, -33 }, { 2873, 26262, 3679, -40 },
    { 2628, 26217, 3971, -48 }, { 2394, 26150, 4276, -56 },
    { 2173, 26061, 4592, -65 }, { 1963, 25950, 4920, -74 },
    { 1764, 25817, 5260, -84 }, { 1576, 25663, 5611, -95 },
    { 1399, 25487, 5974, -106 }, { 1233, 25291, 6347, -118 },
    { 1077, 25075, 6732, -130 }, { 932, 24838, 7127, -143 },
    { 796, 24583, 7532, -156 }, { 671, 24309, 7947, -170 },
    { 554, 24016, 8371, -184 }, { 446, 23706, 8804, -198 },
    { 347, 23379, 9246, -212 }, { 257, 23036, 9696, -226 },
    { 174, 22678, 10153, -240 }, { 99, 22304, 10618, -254 },
    { 31, 21917, 11088, -268 }, { -30, 21517, 11564, -280 },
    { -84, 21104, 12045, -293 }, { -132, 20679, 12531, -304 },
    { -173, 20244, 13020, -314 }, { -210, 19799, 13512, -323 },
    { -241, 19345, 14006, -330 }, { -267, 18882, 14501, -336 },
    { -289, 18413, 14997, -340 }, { -306, 17937, 15493, -341 },
    { -320, 17456, 15988, -340 }, { -330, 16970, 16480, -337 },
    { -337, 16480, 16970, -330 }, { -340, 15988, 17456, -320 },
    { -341, 15493, 17937, -306 }, { -340, 14997, 18413, -289 },
    { -336, 14501, 18882, -267 }, { -330, 14006, 19345, -241 },
    { -323, 13512, 19799, -210 }, { -314, 13020, 20244, -173 },
    { -304, 12531, 20679, -132 }, { -293, 12045, 21104, -84 },
    { -280, 11564, 21517, -30 }, { -268, 11088, 21917, 31 },
    { -254, 10618, 22304, 99 }, { -240, 10153, 22678, 174 },
    { -226, 9696, 23036, 257 }, { -212, 9246, 23379, 347 },
    { -198, 8804, 23706, 446 }, { -184, 8371, 24016, 554 },
    { -170, 7947, 24309, 671 }, { -156, 7532, 24583, 796 },
    { -143, 7127, 24838, 932 }, { -130, 6732, 25075, 1077 },
    { -118, 6347, 25291, 1233 }, { -106, 5974, 25487, 1399 },
    { -95, 5611, 25663, 1576 }, { -84, 5260, 25817, 1764 },
    { -74, 4920, 25950, 1963 }, { -65, 4592, 26061, 2173 },
    { -56, 4276, 26150, 2394 }, { -48, 3971, 26217, 2628 },
    { -40, 3679, 26262, 2873 }, { -33, 3398, 26285, 3129 },
};

/* ---- Nybble-to-float LUT for 4-bit ADPCM (256 byte pairs) ---- */
static float nyblls_as_floats[256][2];
/* ---- Dibit-to-float LUT for 2-bit ADPCM (256 byte quads) ---- */
static float dibits_as_floats[256][4];
static int lut_initialized = 0;

static void init_luts(void) {
    if (lut_initialized) return;
    static const float nybval[16] = {0,1,2,3,4,5,6,7,-8,-7,-6,-5,-4,-3,-2,-1};
    static const float dibval[4] = {0, 1, -2, -1};
    for (int b = 0; b < 256; b++) {
        nyblls_as_floats[b][0] = nybval[b >> 4];
        nyblls_as_floats[b][1] = nybval[b & 0xf];
        dibits_as_floats[b][0] = dibval[(b >> 6) & 3];
        dibits_as_floats[b][1] = dibval[(b >> 4) & 3];
        dibits_as_floats[b][2] = dibval[(b >> 2) & 3];
        dibits_as_floats[b][3] = dibval[b & 3];
    }
    lut_initialized = 1;
}

static inline float shift_to_float(u8 shift) {
    static const float tbl[16] = {
        1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f,
        256.0f, 512.0f, 1024.0f, 2048.0f, 4096.0f, 8192.0f, 16384.0f, 32768.0f
    };
    return tbl[shift & 0xf];
}

static inline s16 clamp16f(float v) {
    return (s16)(v < -32768.0f ? -32768.0f : v > 32767.0f ? 32767.0f : v);
}

static inline float dot4f(float a0, float a1, float a2, float a3, float b0, float b1, float b2, float b3) {
    return a0 * b0 + a1 * b1 + a2 * b2 + a3 * b3;
}

/* ---- Utility functions ---- */

static inline s16 clamp16(s32 v) {
    if (v < -0x8000) return -0x8000;
    if (v > 0x7fff) return 0x7fff;
    return (s16)v;
}

/* ---- Basic buffer operations ---- */

void aClearBufferImpl(u16 addr, int nbytes) {
    nbytes = ROUND_UP_16(nbytes);
    memset(BUF_U8(addr), 0, nbytes);
}

u8 *bufPtr;
int gS30MixDbg[4];

void aLoadBufferImpl(const void* source_addr, u16 dest_addr, u16 nbytes, u8 copy) {
    if (!copy) {
        bufPtr = (u8*)source_addr;
    } else {
        memcpy(BUF_U8(dest_addr), source_addr, ROUND_DOWN_16(nbytes));
        if (copy == 2) {
            bufPtr = rspa.buf; // the decoders read DMEM
        }
    }
}

void aSaveBufferImpl(u16 source_addr, s16* dest_addr, u16 nbytes) {
    memcpy(dest_addr, BUF_S16(source_addr), ROUND_DOWN_16(nbytes));
}
#define recip2048 0.00048828f
void aLoadADPCMImpl(int num_entries_times_16, const s16* book_source_addr) {
    /* Convert ADPCM book to float, pre-divided by 2048 (bakes in the >>11 shift) */
    /* Book data is already byte-swapped to native endian by AudioLoad_ByteSwapAdpcmBook */
    init_luts();
    float* fp = (float*)rspa.adpcm_table;
    if (num_entries_times_16 / 2 > 16 * 2 * 8) {
        printf("mixer: LoadADPCM %d entries (max %d)\n", num_entries_times_16 / 2, 16 * 2 * 8);
        num_entries_times_16 = 16 * 2 * 8 * 2;
    }
    for (int i = 0; i < num_entries_times_16 / 2; i++) {
        fp[i] = recip2048 * (float)(s32)book_source_addr[i];
    }
    memcpy(rspa.adpcm_raw, book_source_addr, num_entries_times_16);
}

void aSetBufferImpl(u8 flags, u16 in, u16 out, u16 nbytes) {
    rspa.in = in;
    rspa.out = out;
    rspa.nbytes = nbytes;
}

/* Front to back, as the RSP does: ranges may overlap */
void aDMEMMoveImpl(u16 in_addr, u16 out_addr, int nbytes) {
    u8* dst = BUF_U8(out_addr);
    const u8* src = BUF_U8(in_addr);
    int i;

    nbytes = ROUND_UP_16(nbytes);
    if (dst + nbytes <= src || src + nbytes <= dst) {
        memcpy(dst, src, nbytes);
        return;
    }
    for (i = 0; i < nbytes; i++) {
        dst[i] = src[i];
    }
}

void aSetLoopImpl(ADPCM_STATE* adpcm_loop_state) {
    memcpy(rspa.adpcm_loop_state, adpcm_loop_state, 16 * sizeof(s16));
}

/* ---- ADPCM decoder ---- */
/* Float version with 2-bit ADPCM support */

void aADPCMdecImpl(u8 flags, ADPCM_STATE state) {
    u8* in = bufPtr + rspa.in;//BUF_U8(rspa.in);
    s16* out = BUF_S16(rspa.out);
    int nbytes = ROUND_UP_32(rspa.nbytes);
#if 0
    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(s16));
    } else if (flags & A_LOOP) {
        memcpy(out, rspa.adpcm_loop_state, 16 * sizeof(s16));
    } else {
        memcpy(out, state, 16 * sizeof(s16));
    }
#endif
    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(s16));
    } else if (flags & A_LOOP) {
        memcpy(out, rspa.adpcm_loop_state, 16 * sizeof(s16));
    } else {
        memcpy(out, state, 16 * sizeof(s16));
    }

    out += 16;

    __builtin_prefetch(in);

    float prev1 = (float)out[-1];
    float prev2 = (float)out[-2];

    while (nbytes > 0) {
        const u8 header = *in++;
        const unsigned table_index = header & 0xf;
        const float shift = shift_to_float(header >> 4);
        const float(*tbl)[8] = rspa.adpcm_table[table_index];
        float instr[2][8];

        __builtin_prefetch(tbl);

        /* Unpack input samples to float based on ADPCM mode */
        if (flags & 4) {
            /* 2-bit ADPCM: 2 bytes per half = 4 bytes total */
            for (int i = 0; i < 2; i++) {
                const float* d0 = dibits_as_floats[*in++];
                const float* d1 = dibits_as_floats[*in++];
                instr[i][0] = d0[0] * shift; instr[i][1] = d0[1] * shift;
                instr[i][2] = d0[2] * shift; instr[i][3] = d0[3] * shift;
                instr[i][4] = d1[0] * shift; instr[i][5] = d1[1] * shift;
                instr[i][6] = d1[2] * shift; instr[i][7] = d1[3] * shift;
            }
        } else {
            /* 4-bit ADPCM: 4 bytes per half = 8 bytes total */
            for (int i = 0; i < 2; i++) {
                for (int j = 0; j < 4; j++) {
                    const float* nyb = nyblls_as_floats[*in++];
                    instr[i][j * 2]     = nyb[0] * shift;
                    instr[i][j * 2 + 1] = nyb[1] * shift;
                }
            }
        }

        __builtin_prefetch(in);

        /* Decode both halves: VADPCM order-2 predictor. With the book
         * pre-divided by 2048, sample j of a half is
         *   tbl[0][j]*prev2 + tbl[1][j]*prev1 + ins[j]
         *   + sum over k < j of tbl[1][j-1-k]*ins[k] */
        for (int i = 0; i < 2; i++) {
            const float* ins = instr[i];
            float accf[8];

            for (int j = 0; j < 8; j++) {
                float acc = tbl[0][j] * prev2 + tbl[1][j] * prev1 + ins[j];

                for (int k = 0; k < j; k++) {
                    acc += tbl[1][j - 1 - k] * ins[k];
                }
                accf[j] = acc;
            }

            /* Write output samples */
            for (int j = 0; j < 6; j++) {
                *out++ = clamp16f(accf[j]);
            }
            prev2 = (float)clamp16f(accf[6]);
            *out++ = (s16)prev2;
            prev1 = (float)clamp16f(accf[7]);
            *out++ = (s16)prev1;
        }

        nbytes -= 16 * sizeof(s16);
    }

    memcpy(state, out - 16, 16 * sizeof(s16));
    gS30MixDbg[0]++;
    for (s16* p = BUF_S16(rspa.out) + 16; p < out; p++) {
        int v = *p < 0 ? -*p : *p;
        gS30MixDbg[2] = v > gS30MixDbg[2] ? v : gS30MixDbg[2];
    }
}

/* ---- Polyphase FIR resampler ---- */
#if 1
void aResampleImpl(u8 flags, u16 pitch, RESAMPLE_STATE state) {
    s16 tmp[16];
    s16* in_initial = BUF_S16(rspa.in);
    s16* in = in_initial;
    s16* out = BUF_S16(rspa.out);
    int nbytes = ROUND_UP_16(rspa.nbytes);
    u32 pitch_accumulator;
    int i;

    if (flags & A_INIT) {
        memset(tmp, 0, 5 * sizeof(s16));
    } else {
        memcpy(tmp, state, 16 * sizeof(s16));
    }

    if (flags & 2) {
        memcpy(in - 8, tmp + 8, 8 * sizeof(s16));
        in -= tmp[5] / sizeof(s16);
    }

    in -= 4;
    pitch_accumulator = (u16)tmp[4];
    memcpy(in, tmp, 4 * sizeof(s16));

    __builtin_prefetch(in);

    do {
        __builtin_prefetch(out);
        for (i = 0; i < 8; i++) {
            float* tbl_f = resample_table_f[pitch_accumulator >> 10];
            float sample_f = dot4f(
                (float)in[0], (float)in[1], (float)in[2], (float)in[3],
                tbl_f[0], tbl_f[1], tbl_f[2], tbl_f[3]) * (1.0f / 32768.0f);
            *out++ = clamp16f(sample_f);
            pitch_accumulator += (pitch << 1);
            in += pitch_accumulator >> 16;
            pitch_accumulator &= 0xFFFF;
        }
        __builtin_prefetch(in);
        nbytes -= 8 * sizeof(s16);
    } while (nbytes > 0);

    state[4] = (s16)pitch_accumulator;
    memcpy(state, in, 4 * sizeof(s16));
    i = (in - in_initial + 4) & 7;
    in -= i;
    if (i != 0) {
        i = -8 - i;
    }
    state[5] = i;
    memcpy(state + 8, in, 8 * sizeof(s16));
}
#endif
#define recip32768 0.00003052f
#define MEM_BARRIER() asm volatile("" : : : "memory");
#define MEM_BARRIER_PREF(ptr) asm volatile("pref @%0" : : "r"((ptr)) : "memory")
#if 0
void aResampleImpl(u8 flags, u16 pitch, RESAMPLE_STATE state) {
    s16 __attribute__((aligned(32))) tmp[32] = { 0 };
    s16* in_initial = BUF_S16(rspa.in);
    s16* in = in_initial;
    MEM_BARRIER_PREF(in);
    s16* out = BUF_S16(rspa.out);
    int nbytes = ROUND_UP_16(rspa.nbytes);
    u32 pitch_accumulator = 0;
    int i = 0;
    float* tbl_f = NULL;
    float sample_f = 0;
    size_t l;

    s16 *dp, *sp;
    s32 *wdp, *wsp;

    if (!(flags & A_INIT)) {
        dp = tmp;
        sp = state;

        wdp = (s32*) dp;
        wsp = (s32*) sp;

        if ((((uintptr_t) wdp | (uintptr_t) wsp) & 3) == 0) {
            for (l = 0; l < 8; l++)
                *wdp++ = *wsp++;
        } else {
            for (l = 0; l < 16; l++)
                *dp++ = *sp++;
        }
    }

    in -= 4;
    pitch_accumulator = (u16) tmp[4];
    tbl_f = /* rspa. */resample_table_f[pitch_accumulator >> 10];
    __builtin_prefetch(tbl_f);

    dp = in;
    sp = tmp;
    for (l = 0; l < 4; l++)
        *dp++ = *sp++;

    do {
        __builtin_prefetch(out);
        for (i = 0; i < 8; i++) {

            float in_f[4] = { (float) (int) in[0], (float) (int) in[1], (float) (int) in[2], (float) (int) in[3] };

            sample_f =
                dot4f(in_f[0], in_f[1], in_f[2], in_f[3], tbl_f[0], tbl_f[1], tbl_f[2], tbl_f[3]) * recip32768;

            MEM_BARRIER();
            pitch_accumulator += (pitch << 1);
            in += pitch_accumulator >> 16;
            MEM_BARRIER_PREF(in);
            pitch_accumulator &= 0xffff; //%= 0x10000;
            MEM_BARRIER();
            *out++ = clamp16f(sample_f);
            MEM_BARRIER();
            tbl_f = /* rspa. */resample_table_f[pitch_accumulator >> 10];
            MEM_BARRIER_PREF(tbl_f);
        }
        nbytes -= 8 * sizeof(s16);
    } while (nbytes > 0);

    state[4] = (s16) pitch_accumulator;
    dp = (s16*) (state);
    sp = in;
    for (l = 0; l < 4; l++)
        *dp++ = *sp++;

    i = (in - in_initial + 4) & 7;
    in -= i;
    if (i != 0) {
        i = -8 - i;
    }
    state[5] = i;
    dp = (s16*) (state + 8);
    sp = in;
    for (l = 0; l < 8; l++)
        *dp++ = *sp++;
}
#endif
/* ---- Envelope setup (OoT stereo version) ---- */

void aEnvSetup1Impl(u8 initialVolReverb, u16 rampReverb, u16 rampLeft, u16 rampRight) {
    rspa.vol_wet = (u16)(initialVolReverb << 8);
    rspa.rate_wet = rampReverb;
    rspa.rate[0] = rampLeft;
    rspa.rate[1] = rampRight;
}

void aEnvSetup2Impl(u16 initialVolLeft, u16 initialVolRight) {
    rspa.vol[0] = initialVolLeft;
    rspa.vol[1] = initialVolRight;
}

/* ---- Envelope mixer (OoT version with 4 output destinations) ---- */

/* Each sample goes to the dry pair at the left/right volumes and to the wet
   (reverb send) pair at the reverb volume; swapReverb crosses the wet sends.
   x0..x3 invert channels (headset/surround effects): x2 and x3 the dry left
   and right, x0 and x1 the wet right and left. */
void aEnvMixerImpl(u16 dmemIn, u16 count, u8 swapReverb, u8 x0, u8 x1,
                   u8 x2, u8 x3, u32 dmemDests, u32 opBits) {
    s16* in = BUF_S16(dmemIn);
    s16* dry[2] = { BUF_S16(((dmemDests >> 24) & 0xFF) << 4), BUF_S16(((dmemDests >> 16) & 0xFF) << 4) };
    s16* wet[2] = { BUF_S16(((dmemDests >> 8) & 0xFF) << 4), BUF_S16((dmemDests & 0xFF) << 4) };
    s16 negs[4] = { x2 ? -1 : 0, x3 ? -1 : 0, x0 ? -4 : 0, x1 ? -2 : 0 };
    int swapped[2] = { swapReverb ? 1 : 0, swapReverb ? 0 : 1 };
    u16 vols[2] = { rspa.vol[0], rspa.vol[1] };
    u16 rates[2] = { rspa.rate[0], rspa.rate[1] };
    u16 volWet = rspa.vol_wet;
    u16 rateWet = rspa.rate_wet;
    int n = ROUND_UP_16(count);

    (void)opBits;
    do {
        for (int i = 0; i < 8; i++) {
            s16 samples[2] = { *in, *in };

            in++;
            for (int j = 0; j < 2; j++) {
                samples[j] = (s16)((samples[j] * vols[j] >> 16) ^ negs[j]);
            }
            for (int j = 0; j < 2; j++) {
                *dry[j] = clamp16(*dry[j] + samples[j]);
                dry[j]++;
                *wet[j] = clamp16(*wet[j] + ((samples[swapped[j]] * volWet >> 16) ^ negs[2 + j]));
                wet[j]++;
            }
        }
        vols[0] += rates[0];
        vols[1] += rates[1];
        volWet += rateWet;
        n -= 8;
    } while (n > 0);
    gS30MixDbg[1]++;
    gS30MixDbg[3] = vols[0] > gS30MixDbg[3] ? vols[0] : gS30MixDbg[3];
}

/* ---- Interleave (stereo output from separate L/R) ---- */
#if 0
void aInterleaveImpl(u16 outAddr, u16 inL, u16 inR, u16 count) {
    s16* left  = BUF_S16(inL);
    s16* right = BUF_S16(inR);
    s16* out   = BUF_S16(outAddr);
    int n = count / 2;

    __builtin_prefetch(left);
    __builtin_prefetch(right);

    for (int i = 0; i < n; i++) {
        *out++ = left[i];
        *out++ = right[i];
    }
}
#endif
/* ---- Deinterleave (take every other sample) ---- */
#if 0
void aInterlImpl(u16 in_addr, u16 out_addr, u16 n_samples) {
    s16* in  = BUF_S16(in_addr);
    s16* out = BUF_S16(out_addr);
    int n = ROUND_UP_8(n_samples);

    __builtin_prefetch(in);

    do {
        __builtin_prefetch(out);
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        __builtin_prefetch(in);
        n -= 8;
    } while (n > 0);
}
#endif

void aInterlImpl(u16 in_addr, u16 out_addr, u16 n_samples) {
    s16* in  = BUF_S16(in_addr);
    s16* out = BUF_S16(out_addr);
    int n = ROUND_UP_8(n_samples);

    __builtin_prefetch(out);

    do {
        __builtin_prefetch(out);
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;
        *out++ = *in++; in++;

        __builtin_prefetch(in);
        n -= 8;
    } while (n > 0);
}

/* ---- Mix: out = ((out * 0x7FFF) + (in * gain) + 0x4000) >> 15 ---- */

void aMixImpl(u16 count, s16 gain, u16 in_addr, u16 out_addr) {
    int nbytes = ROUND_UP_32(ROUND_DOWN_16(count << 4));
    s16* in  = BUF_S16(in_addr);
    s16* out = BUF_S16(out_addr);
    int i;
    s32 sample;

    __builtin_prefetch(in);
    __builtin_prefetch(out);

    if (gain == -0x8000) {
        while (nbytes > 0) {
            __builtin_prefetch(in + 16);
            __builtin_prefetch(out + 16);
            for (i = 0; i < 16; i++) {
                sample = *out - *in++;
                *out++ = clamp16(sample);
            }
            nbytes -= 16 * sizeof(s16);
        }
        return;
    }

    while (nbytes > 0) {
        __builtin_prefetch(in + 16);
        __builtin_prefetch(out + 16);
        for (i = 0; i < 16; i++) {
            sample = ((*out * 0x7fff + *in++ * gain) + 0x4000) >> 15;
            *out++ = clamp16(sample);
        }
        nbytes -= 16 * sizeof(s16);
    }
}

/* ---- 8-tap filter for reverb ---- */

void aFilterImpl(u8 flags, u16 count_or_buf, s16* state_or_filter) {
    if (flags > A_INIT) {
        /* Load filter coefficients and store count */
        if ((uintptr_t)state_or_filter < 0x10000) {
            printf("mixer: Filter flags %d loads coefficients from %p\n", flags, state_or_filter);
            return;
        }
        rspa.filter_count = ROUND_UP_16(count_or_buf);
        memcpy(rspa.filter, state_or_filter, sizeof(rspa.filter));
    } else {
        s16 tmp[16], tmp2[8];
        int count = rspa.filter_count;
        s16* buf = BUF_S16(count_or_buf);

        if (flags == A_INIT) {
            memset(tmp, 0, 8 * sizeof(s16));
            memset(tmp2, 0, 8 * sizeof(s16));
        } else {
            memcpy(tmp, state_or_filter, 8 * sizeof(s16));
            memcpy(tmp2, state_or_filter + 8, 8 * sizeof(s16));
        }

        /* Average coefficients with saved state */
        for (int i = 0; i < 8; i++) {
            rspa.filter[i] = (tmp2[i] + rspa.filter[i]) / 2;
        }

        __builtin_prefetch(buf);

        do {
            memcpy(tmp + 8, buf, 8 * sizeof(s16));
            __builtin_prefetch(buf + 8);
            for (int i = 0; i < 8; i++) {
                s64 sample = 0x4000; /* round term */
                for (int j = 0; j < 8; j++) {
                    sample += tmp[i + j] * rspa.filter[7 - j];
                }
                buf[i] = clamp16((s32)(sample >> 15));
            }
            memcpy(tmp, tmp + 8, 8 * sizeof(s16));

            buf += 8;
            count -= 8 * sizeof(s16);
        } while (count > 0);

        memcpy(state_or_filter, tmp, 8 * sizeof(s16));
        memcpy(state_or_filter + 8, rspa.filter, 8 * sizeof(s16));
    }
}

/* ---- HiLo gain: samples * g (UQ4.4), saturated ---- */

void aHiLoGainImpl(u8 g, u16 count, u16 addr) {
    s16* samples = BUF_S16(addr);
    int nbytes = ROUND_UP_32(count);

    do {
        for (int i = 0; i < 8; i++) {
            samples[i] = clamp16((samples[i] * g) >> 4);
        }
        samples += 8;
        nbytes -= 8 * sizeof(s16);
    } while (nbytes > 0);
}

/* ---- Duplicate 128-byte blocks ---- */

void aDuplicateImpl(u16 count, u16 in_addr, u16 out_addr) {
    u8* in  = BUF_U8(in_addr);
    u8* out = BUF_U8(out_addr);
//    u8 tmp[128];

//    memcpy(tmp, in, 128);
    do {
        memcpy(out, in/* tmp */, 128);
        out += 128;
    } while (count-- > 0);
}

/* ---- Additive mixer with clamping (for Haas effect) ---- */

void aAddMixerImpl(u16 count, u16 in_addr, u16 out_addr, u16 a4) {
    (void)a4; /* Unused - RSP does pure saturating add */
    s16* in  = BUF_S16(in_addr);
    s16* out = BUF_S16(out_addr);
    int nbytes = ROUND_UP_64(ROUND_DOWN_16(count));

    __builtin_prefetch(in);
    __builtin_prefetch(out);

    do {
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        *out = clamp16(*out + *in++); out++;
        __builtin_prefetch(in);
        __builtin_prefetch(out);
        nbytes -= 16 * sizeof(s16);
    } while (nbytes > 0);
}

/* ---- S8 decode: 8-bit signed to 16-bit ---- */

void aS8DecImpl(u8 flags, ADPCM_STATE state) {
    u8* in = bufPtr + rspa.in;//BUF_U8(rspa.in);
    s16* out = BUF_S16(rspa.out);
    int nbytes = ROUND_UP_32(rspa.nbytes);

    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(s16));
    } else if (flags & A_LOOP) {
        memcpy(out, rspa.adpcm_loop_state, 16 * sizeof(s16));
    } else {
        memcpy(out, state, 16 * sizeof(s16));
    }
    out += 16;

    while (nbytes > 0) {
        *out++ = (s16)(*in++ << 8);
        *out++ = (s16)(*in++ << 8);
        *out++ = (s16)(*in++ << 8);
        *out++ = (s16)(*in++ << 8);
        *out++ = (s16)(*in++ << 8);
        *out++ = (s16)(*in++ << 8);
        *out++ = (s16)(*in++ << 8);
        *out++ = (s16)(*in++ << 8);
        *out++ = (s16)(*in++ << 8);
        *out++ = (s16)(*in++ << 8);
        *out++ = (s16)(*in++ << 8);
        *out++ = (s16)(*in++ << 8);
        *out++ = (s16)(*in++ << 8);
        *out++ = (s16)(*in++ << 8);
        *out++ = (s16)(*in++ << 8);
        *out++ = (s16)(*in++ << 8);
        nbytes -= 16 * sizeof(s16);
    }

    memcpy(state, out - 16, 16 * sizeof(s16));
}

/* ---- Zero-order hold resample (for Haas pitch adjustment) ---- */

void aResampleZohImpl(u16 pitch, u16 start_fract) {
    s16* in  = BUF_S16(rspa.in);
    s16* out = BUF_S16(rspa.out);
    int nbytes = ROUND_UP_8(rspa.nbytes);
    u32 pos = start_fract;
    u32 pitch_add = pitch << 2;

    do {
        *out++ = in[pos >> 17];
        pos += pitch_add;
        *out++ = in[pos >> 17];
        pos += pitch_add;
        *out++ = in[pos >> 17];
        pos += pitch_add;
        *out++ = in[pos >> 17];
        pos += pitch_add;
        nbytes -= 4 * sizeof(s16);
    } while (nbytes > 0);
}

/* ---- Unknown commands ---- */

void aUnkCmd3Impl(u16 a, u16 b, u16 c) {
    (void)a; (void)b; (void)c;
}

void aUnkCmd19Impl(u8 f, u16 count, u16 out_addr, u16 in_addr) {
    int nbytes = ROUND_UP_64(count);
    s16* in  = BUF_S16(in_addr + f);
    s16* out = BUF_S16(out_addr);
    s16 tbl[32];

    memcpy(tbl, in, 32 * sizeof(s16));
    do {
        for (int i = 0; i < 32; i++) {
            out[i] = clamp16(out[i] * tbl[i]);
        }
        out += 32;
        nbytes -= 32 * sizeof(s16);
    } while (nbytes > 0);
}

/* Star Fox's interleave: to the aSetBuffer output, aSetBuffer's byte count
   per channel */
/* Interleave: the left and right channels into the AI buffer. count is the
   size of each channel in bytes; the output (2 * count bytes) may overlap the
   inputs (MM writes it to DMEM_TEMP), so it is built aside */
void aInterleaveImpl(u16 outAddr, u16 inL, u16 inR, u16 count) {
    static s16 tmp[DMEM_BUF_SIZE / 2];
    const s16* l = BUF_S16(inL);
    const s16* r = BUF_S16(inR);
    int n = ROUND_UP_16(count) / 2;
    int i;

    if (outAddr + 4 * n > DMEM_BUF_SIZE) {
        n = (DMEM_BUF_SIZE - outAddr) / 4;
    }
    for (i = 0; i < n; i++) {
        tmp[2 * i] = l[i];
        tmp[2 * i + 1] = r[i];
    }
    memcpy(BUF_S16(outAddr), tmp, 4 * n);
}

/* Star Fox's interleave: output and size from the last aSetBuffer */
void aInterleaveSetBufferImpl(u16 inL, u16 inR) {
    aInterleaveImpl(rspa.out, inL, inR, rspa.nbytes);
}

/* ---- The SM64 EU / Mario Kart 64 microcode's own commands ---- */

/* Envelope mixer of the Mario Kart 64 microcode (mupen64plus-rsp-hle's
   ENVMIXER_MK): each sample to the dry left/right at their volumes, the
   flags inverting either (XOR -1), and the wet left/right sends taken from
   those at the reverb volume. count is in samples (the command's 8 bits),
   rounded up to 8. */
void aEnvMixerEuImpl(u16 dmemIn, u16 count, u8 swapReverb, u8 negLeft, u8 negRight, u16 dryLeft, u16 dryRight,
                     u16 wetLeft, u16 wetRight) {
    s16* in = BUF_S16(dmemIn);
    s16* dry[2] = { BUF_S16(dryLeft), BUF_S16(dryRight) };
    s16* wet[2] = { BUF_S16(wetLeft), BUF_S16(wetRight) };
    s16 xors[2] = { negLeft ? -1 : 0, negRight ? -1 : 0 };
    u16 vols[2] = { rspa.vol[0], rspa.vol[1] };
    u16 volWet = rspa.vol_wet;
    int n = ROUND_UP_8(count);

    (void)swapReverb; // not in this microcode
    while (n > 0) {
        for (int i = 0; i < 8; i++) {
            s32 x = *in++;

            for (int j = 0; j < 2; j++) {
                s16 d = (s16)((x * vols[j]) >> 16) ^ xors[j];

                *dry[j] = clamp16(*dry[j] + d);
                dry[j]++;
                *wet[j] = clamp16(*wet[j] + (s16)((d * volWet) >> 16));
                wet[j]++;
            }
        }
        vols[0] += rspa.rate[0];
        vols[1] += rspa.rate[1];
        n -= 8;
    }
    gS30MixDbg[1]++;
    gS30MixDbg[3] = vols[0] > gS30MixDbg[3] ? vols[0] : gS30MixDbg[3];
}

/* times x count bytes (count rounded up to 32) from in to out, 32 at a time
   going up: with out = in + count it repeats the first count bytes */
void aDMEMMove2Impl(u8 times, u16 in_addr, u16 out_addr, u16 count) {
    u8* in = BUF_U8(in_addr);
    u8* out = BUF_U8(out_addr);
    int n = times * ROUND_UP_32(count);

    for (int i = 0; i < n; i += 32) {
        memcpy(out + i, in + i, 32);
    }
}

/* Every other sample: count output samples (rounded up to 8) */
void aDownsampleHalfImpl(u16 count, u16 in_addr, u16 out_addr) {
    s16* in = BUF_S16(in_addr);
    s16* out = BUF_S16(out_addr);
    int n = ROUND_UP_8(count);

    for (int i = 0; i < n; i++) {
        out[i] = in[2 * i];
    }
}

/* Buffer load/save with the RSP DMA's own rounding (the count up to 8 bytes,
   the DRAM address down to 8): the Mario Kart 64 microcode's LOADBUFF and
   SAVEBUFF. Its reverb chunks aren't multiples of 16 bytes. The decoders
   read the copy in DMEM. */
void aLoadBuffer8Impl(const void* source_addr, u16 dest_addr, u16 nbytes) {
    memcpy(BUF_U8(dest_addr & ~3), (const u8*)((uintptr_t)source_addr & ~(uintptr_t)7), ROUND_UP_8(nbytes));
    bufPtr = rspa.buf;
}

void aSaveBuffer8Impl(u16 source_addr, void* dest_addr, u16 nbytes) {
    memcpy((u8*)((uintptr_t)dest_addr & ~(uintptr_t)7), BUF_U8(source_addr & ~3), ROUND_UP_8(nbytes));
}

/* DMEM move of the Mario Kart 64 microcode: the count up to 4 bytes, copied
   going up a byte at a time (the ADPCM loop moves samples down over
   themselves) */
void aDMEMMove4Impl(u16 in_addr, u16 out_addr, u16 nbytes) {
    u8* in = BUF_U8(in_addr);
    u8* out = BUF_U8(out_addr);
    int n = (nbytes + 3) & ~3;

    for (int i = 0; i < n; i++) {
        out[i] = in[i];
    }
}

/* ---- libultra 2.0's aspMain (ABI1): Diddy Kong Racing's libultra audio ---- */
/* After mupen64plus-rsp-hle's alist_audio.c and alist.c (alist_process_audio_bc:
   ENVMIXER is alist_envmix_ge). DMEM addresses are the library's, without the
   microcode's DMEM base (0x5C0): they fit this DMEM as they are. */

void aSetBufferAbi1Impl(u8 flags, u16 in, u16 out, u16 count) {
    if (flags & 0x08) { // A_AUX
        rspa.dry_right = in;
        rspa.wet_left = out;
        rspa.wet_right = count;
    } else {
        aSetBufferImpl(flags, in, out, count);
    }
}

/* The count from the last aSetBuffer; the decoders read the copy in DMEM */
void aLoadBufferAbi1Impl(const void* source_addr) {
    if (rspa.nbytes != 0) {
        memcpy(BUF_U8(rspa.in), source_addr, ROUND_UP_8(rspa.nbytes));
    }
    bufPtr = rspa.buf;
}

void aSaveBufferAbi1Impl(void* dest_addr) {
    if (rspa.nbytes != 0) {
        memcpy(dest_addr, BUF_U8(rspa.out), ROUND_UP_8(rspa.nbytes));
    }
}

void aSetVolumeAbi1Impl(u8 flags, s16 v, s16 t, s16 r) {
    if (flags & 0x08) { // A_AUX: dry and wet amounts
        rspa.dry = v;
        rspa.wet = r;
    } else {
        int lr = (flags & 0x02) ? 0 : 1; // A_LEFT

        if (flags & 0x04) { // A_VOL
            rspa.env_vol[lr] = v;
        } else {
            rspa.env_target[lr] = v;
            rspa.env_rate[lr] = (s32)(((u32)(u16)t << 16) | (u16)r);
        }
    }
}

typedef struct {
    s64 value, step, target;
} Ramp;

static s16 ramp_step(Ramp* ramp) {
    int reached;

    ramp->value += ramp->step;
    reached = ramp->step <= 0 ? ramp->value <= ramp->target : ramp->value >= ramp->target;
    if (reached) {
        ramp->value = ramp->target;
        ramp->step = 0;
    }
    return (s16)(ramp->value >> 16);
}

static void sample_mix(s16* dst, s16 src, s16 gain) {
    *dst = clamp16(*dst + ((src * gain) >> 15));
}

/* Linear volume ramps towards the targets, a step a sample (the Blast Corps /
   Diddy Kong Racing and GoldenEye aspMain, mupen64plus-rsp-hle's
   alist_envmix_ge; the common aspMain's exponential ramp, alist_envmix_exp, is
   not implemented): dry to the output and dry right, wet (with A_AUX) to the
   wet left and right. The state (40 halves) holds the ramps between calls, in
   this mixer's own layout. */
void aEnvMixerAbi1Impl(u8 flags, s16* state) {
    const s16* in = BUF_S16(rspa.in);
    s16* out[4] = { BUF_S16(rspa.out), BUF_S16(rspa.dry_right), BUF_S16(rspa.wet_left), BUF_S16(rspa.wet_right) };
    int n = (flags & 0x08) ? 4 : 2;
    s16 dry = rspa.dry, wet = rspa.wet;
    Ramp ramps[2];
    s32 saved[8];
    int count = rspa.nbytes / 2, i, k;

    if (flags & A_INIT) {
        for (k = 0; k < 2; k++) {
            ramps[k].value = (s64)rspa.env_vol[k] << 16;
            ramps[k].target = (s64)rspa.env_target[k] << 16;
            ramps[k].step = rspa.env_rate[k] / 8;
        }
    } else {
        memcpy(saved, state, sizeof(saved));
        wet = (s16)saved[0];
        dry = (s16)saved[1];
        for (k = 0; k < 2; k++) {
            ramps[k].target = saved[2 + k];
            ramps[k].step = saved[4 + k];
            ramps[k].value = saved[6 + k];
        }
    }
    for (i = 0; i < count; i++) {
        s16 lvol = ramp_step(&ramps[0]);
        s16 rvol = ramp_step(&ramps[1]);
        s16 gains[4];

        gains[0] = clamp16((lvol * dry + 0x4000) >> 15);
        gains[1] = clamp16((rvol * dry + 0x4000) >> 15);
        gains[2] = clamp16((lvol * wet + 0x4000) >> 15);
        gains[3] = clamp16((rvol * wet + 0x4000) >> 15);
        for (k = 0; k < n; k++) {
            sample_mix(&out[k][i], in[i], gains[k]);
        }
    }
    saved[0] = wet;
    saved[1] = dry;
    for (k = 0; k < 2; k++) {
        saved[2 + k] = (s32)ramps[k].target;
        saved[4 + k] = (s32)ramps[k].step;
        saved[6 + k] = (s32)ramps[k].value;
    }
    memcpy(state, saved, sizeof(saved));
    gS30MixDbg[1]++;
}

/* The count from the last aSetBuffer, rounded up to 32 bytes */
void aMixAbi1Impl(s16 gain, u16 in_addr, u16 out_addr) {
    const s16* in = BUF_S16(in_addr);
    s16* out = BUF_S16(out_addr);
    int n = ROUND_UP_32(rspa.nbytes) / 2, i;

    for (i = 0; i < n; i++) {
        sample_mix(&out[i], in[i], gain);
    }
}

static s32 rdot(int n, const s16* x, const s16* y) {
    s32 accu = 0;

    y += n;
    while (n-- > 0) {
        accu += *x++ * *--y;
    }
    return accu;
}

/* The two-pole filter: its coefficients are the ADPCM book loaded last (h1,
   then h2, which the gain scales in place, as the microcode does); the state
   keeps the last 4 outputs */
void aPoleFilterAbi1Impl(u8 flags, u16 gain, s16* state) {
    s16* dst = BUF_S16(rspa.out);
    const s16* src = BUF_S16(rspa.in);
    const s16* h1 = rspa.adpcm_raw;
    s16* h2 = rspa.adpcm_raw + 8;
    s16 h2_before[8];
    s16 l1, l2;
    int count = ROUND_UP_16(rspa.nbytes), i;

    if (count == 0) {
        return;
    }
    if (flags & A_INIT) {
        l1 = 0;
        l2 = 0;
    } else {
        l1 = state[2];
        l2 = state[3];
    }
    for (i = 0; i < 8; i++) {
        h2_before[i] = h2[i];
        h2[i] = (s16)(((s32)h2[i] * gain) >> 14);
    }
    do {
        s16 frame[8];

        for (i = 0; i < 8; i++) {
            frame[i] = *src++;
        }
        for (i = 0; i < 8; i++) {
            s32 accu = frame[i] * (s32)gain;

            accu += h1[i] * l1 + h2_before[i] * l2 + rdot(i, h2, frame);
            dst[i] = clamp16(accu >> 14);
        }
        l1 = dst[6];
        l2 = dst[7];
        dst += 8;
        count -= 16;
    } while (count > 0);
    memcpy(state, dst - 4, 4 * sizeof(s16));
}

