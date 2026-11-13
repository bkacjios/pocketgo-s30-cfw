/*
 * s30-mdns: answers mDNS queries for NAME.local on one interface with that
 * interface's IPv4 address, so the host reaches the device as s30.local
 * over the USB network instead of 10.42.0.2 (Linux with systemd-resolved
 * or avahi, macOS and Windows 10+ resolve .local themselves). Only A
 * records: no service browsing, no AAAA. Announces the address at start,
 * answers queries (multicast, or unicast when the query asks for it or
 * comes from a legacy resolver), and sends a goodbye (TTL 0) on SIGTERM.
 *
 * Usage: s30-mdns IFACE [NAME]   (NAME defaults to the hostname)
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define MDNS_PORT 5353
#define MDNS_GROUP "224.0.0.251"
#define TTL 120
#define ANNOUNCEMENTS 3

static const char *sIface;
static char sName[64]; // "s30.local" in DNS label form: \003s30\005local
static int sNameLen;
static int sSock;
static volatile sig_atomic_t sQuit;

static void onSignal(int sig) { (void)sig; sQuit = 1; }

// The interface's current IPv4 address, 0 if it has none
static uint32_t ifaceAddr(void)
{
	struct ifreq ifr;
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, sIface, IFNAMSIZ - 1);
	if (ioctl(sSock, SIOCGIFADDR, &ifr) < 0) return 0;
	return ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
}

static void toLabels(const char *host)
{
	int n = 0;
	const char *parts[2] = { host, "local" };
	for (int i = 0; i < 2; i++) {
		int len = strlen(parts[i]);
		if (len > 63 || n + 1 + len + 1 > (int)sizeof sName) { fprintf(stderr, "s30-mdns: name too long\n"); exit(1); }
		sName[n++] = len;
		memcpy(sName + n, parts[i], len);
		n += len;
	}
	sName[n++] = 0;
	sNameLen = n;
}

// Reads a name at pkt[pos] (compression pointers followed); returns the
// position after it or -1. The expanded name goes to out (label form).
static int readName(const uint8_t *pkt, int len, int pos, char *out, int outSize)
{
	int n = 0, hops = 0, end = -1;
	while (1) {
		if (pos >= len) return -1;
		int l = pkt[pos];
		if (l == 0) { if (n >= outSize) return -1; out[n++] = 0; pos++; break; }
		if ((l & 0xc0) == 0xc0) {
			if (pos + 1 >= len || ++hops > 16) return -1;
			if (end < 0) end = pos + 2;
			pos = ((l & 0x3f) << 8) | pkt[pos + 1];
			continue;
		}
		if (pos + 1 + l > len || n + 1 + l >= outSize) return -1;
		out[n++] = l;
		memcpy(out + n, pkt + pos + 1, l);
		n += l;
		pos += 1 + l;
	}
	return end < 0 ? pos : end;
}

static int nameIs(const char *labels)
{
	for (int i = 0; i < sNameLen; i++)
		if (tolower((unsigned char)labels[i]) != tolower((unsigned char)sName[i])) return 0;
	return 1;
}

// One response: NAME.local A addr, cache-flush bit set
static void respond(uint16_t id, uint32_t addr, uint32_t ttl, const struct sockaddr_in *to)
{
	uint8_t pkt[128];
	int n = 0;
	uint16_t hdr[6] = { htons(id), htons(0x8400), 0, htons(1), 0, 0 };
	memcpy(pkt, hdr, 12);
	n = 12;
	memcpy(pkt + n, sName, sNameLen);
	n += sNameLen;
	uint16_t type = htons(1), cls = htons(0x8001), rdlen = htons(4);
	uint32_t ttln = htonl(ttl);
	memcpy(pkt + n, &type, 2); n += 2;
	memcpy(pkt + n, &cls, 2); n += 2;
	memcpy(pkt + n, &ttln, 4); n += 4;
	memcpy(pkt + n, &rdlen, 2); n += 2;
	memcpy(pkt + n, &addr, 4); n += 4;
	sendto(sSock, pkt, n, 0, (const struct sockaddr *)to, sizeof *to);
}

static void announce(uint32_t ttl)
{
	uint32_t addr = ifaceAddr();
	if (!addr) return;
	struct sockaddr_in group = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT) };
	inet_pton(AF_INET, MDNS_GROUP, &group.sin_addr);
	respond(0, addr, ttl, &group);
}

// Answers the queries in one packet that ask for our name (A or ANY, class IN)
static void handle(const uint8_t *pkt, int len, const struct sockaddr_in *from)
{
	if (len < 12) return;
	uint16_t flags = (pkt[2] << 8) | pkt[3];
	if (flags & 0x8000) return; // a response
	int qd = (pkt[4] << 8) | pkt[5];
	int pos = 12;
	uint32_t addr = 0;
	for (int i = 0; i < qd; i++) {
		char labels[256];
		pos = readName(pkt, len, pos, labels, sizeof labels);
		if (pos < 0 || pos + 4 > len) return;
		int qtype = (pkt[pos] << 8) | pkt[pos + 1];
		int qclass = (pkt[pos + 2] << 8) | pkt[pos + 3];
		pos += 4;
		int unicast = qclass & 0x8000; // the QU bit
		qclass &= 0x7fff;
		if ((qtype != 1 && qtype != 255) || (qclass != 1 && qclass != 255) || !nameIs(labels)) continue;
		if (!addr && !(addr = ifaceAddr())) return;
		// Legacy resolvers (not on port 5353) get a unicast reply with their
		// ID; mDNS ones a multicast reply (ID 0) unless they asked for unicast
		if (ntohs(from->sin_port) != MDNS_PORT) {
			respond((pkt[0] << 8) | pkt[1], addr, 10, from);
		} else if (unicast) {
			respond(0, addr, TTL, from);
		} else {
			struct sockaddr_in group = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT) };
			inet_pton(AF_INET, MDNS_GROUP, &group.sin_addr);
			respond(0, addr, TTL, &group);
		}
	}
}

int main(int argc, char **argv)
{
	if (argc < 2 || argc > 3) { fprintf(stderr, "usage: s30-mdns IFACE [NAME]\n"); return 2; }
	sIface = argv[1];
	char host[64] = "";
	if (argc == 3) snprintf(host, sizeof host, "%s", argv[2]);
	else if (gethostname(host, sizeof host - 1) < 0 || !*host) snprintf(host, sizeof host, "s30");
	toLabels(host);

	unsigned ifindex = if_nametoindex(sIface);
	if (!ifindex) { fprintf(stderr, "s30-mdns: %s: %s\n", sIface, strerror(errno)); return 1; }

	sSock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sSock < 0) { perror("socket"); return 1; }
	int one = 1, ttl = 255;
	setsockopt(sSock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	setsockopt(sSock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
	setsockopt(sSock, IPPROTO_IP, IP_MULTICAST_LOOP, &(int){0}, sizeof(int));
	// Only this interface's traffic
	if (setsockopt(sSock, SOL_SOCKET, SO_BINDTODEVICE, sIface, strlen(sIface) + 1) < 0) { perror("SO_BINDTODEVICE"); return 1; }
	struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT), .sin_addr.s_addr = INADDR_ANY };
	if (bind(sSock, (struct sockaddr *)&local, sizeof local) < 0) { perror("bind 5353"); return 1; }
	struct ip_mreqn mreq = { .imr_ifindex = ifindex };
	inet_pton(AF_INET, MDNS_GROUP, &mreq.imr_multiaddr);
	if (setsockopt(sSock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) < 0) { perror("IP_ADD_MEMBERSHIP"); return 1; }
	setsockopt(sSock, IPPROTO_IP, IP_MULTICAST_IF, &mreq, sizeof mreq);

	signal(SIGTERM, onSignal);
	signal(SIGINT, onSignal);

	// Announce, then serve; the first announcements are interleaved with
	// serving so queries during them aren't missed
	int toAnnounce = ANNOUNCEMENTS;
	while (!sQuit) {
		if (toAnnounce) {
			announce(TTL);
			toAnnounce--;
		}
		struct timeval tv = { .tv_sec = 1 };
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(sSock, &fds);
		int r = select(sSock + 1, &fds, NULL, NULL, toAnnounce ? &tv : NULL);
		if (r < 0) { if (errno == EINTR) continue; perror("select"); break; }
		if (r == 0) continue;
		uint8_t pkt[1500];
		struct sockaddr_in from;
		socklen_t fromLen = sizeof from;
		int len = recvfrom(sSock, pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fromLen);
		if (len > 0) handle(pkt, len, &from);
	}
	announce(0); // goodbye
	return 0;
}
