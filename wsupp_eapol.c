#include <sys/socket.h>
#include <netpacket/packet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include "common.h"

#include "wsupp.h"
#include "wsupp_crypto.h"
#include "wsupp_eapol.h"

/* Once the radio level connection has been established by the NL code,
   there's a usable ethernet-style link to the AP. There's no encryption
   yet however, and the AP does not let any packets through except for
   the EHT_P_PAE (type 0x888E) key negotiation packets.

   The keys are negotiated by sending packets over this ethernet link.
   It's a 4-way handshake, 2 packets in and 2 packets out, and it is
   initiated by the AP. Well actually not, it's initiated by the client
   (that's us) sending the right IEs with the ASSOCIATE command back
   in NL code, but here at EAPOL level it looks like the AP talks first.

   The final result of negotiations is PTK and GTK. */

#define ARPHRD_ETHER 1
#define ETH_P_PAE 0x888E

char* ifname;
int ifindex;
int rawsock;

int eapolstate;
int eapolsends;

static int version;
byte amac[6]; /* A = authenticator (AP) */
byte smac[6]; /* S = supplicant (client) */

byte anonce[32];
byte snonce[32];

byte replay[8];

byte PSK[32];

byte KCK[16]; /* key check key, for computing MICs */
byte KEK[16]; /* key encryption key, for AES unwrapping */
byte PTK[16]; /* pairwise key (just TK in 802.11 terms) */
byte GTK[32]; /* group temporary key */
byte RSC[6];  /* ATTR_KEY_SEQ for GTK */
int gtkindex;

static char packet[1024];

static void send_packet_2(void);
static void send_packet_4(void);
static void send_group_2(void);

/* A socket bound to an interface enters failed state if the interface
   goes down, which happens during rfkill. If this happens, we have to
   re-open adn re-bind it. Otherwise, there's no problem with the socket
   remaining open across connection, so we do not bother closing it. */

static void open_rawsock(void)
{
	int type = htons(ETH_P_PAE);
	int flags = SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC;
	int fd;

	if((fd = socket(AF_PACKET, flags, type)) < 0)
		quit("socket AF_PACKET: %m\n");

	struct sockaddr_ll addr = {
		.sll_family = AF_PACKET,
		.sll_ifindex = ifindex,
		.sll_protocol = type
	};

	if(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
		quit("bind AF_PACKET: %m\n");

	rawsock = fd;
}

void reopen_rawsock(void)
{
	if(rawsock >= 0)
		return;

	open_rawsock();
}

void setup_iface(char* name)
{
	int fd = netlink;
	uint nlen = strlen(name);
	struct ifreq ifr;

	if(nlen > sizeof(ifr.ifr_name))
		fail("name too long: %s\n", name);

	memzero(&ifr, sizeof(ifr));
	memcpy(ifr.ifr_name, name, nlen);

	if(ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
		fail("ioctl SIOCGIFINDEX %s: %m\n", name);

	ifname = name;
	ifindex = ifr.ifr_ifindex;

	if(ioctl(fd, SIOCGIFHWADDR, &ifr) < 0)
		fail("ioctl SIOCGIFHWADDR %s: %m\n", name);

	if(ifr.ifr_addr.sa_family != ARPHRD_ETHER)
		fail("unexpected hwaddr family on %s\n", name);

	memcpy(smac, ifr.ifr_addr.sa_data, 6);

	open_rawsock();
}

/* The rest of the code deals with AP connection */

static void ignore(char* why)
{
	warn("EAPOL %s", why);
}

static void xabort(char* why)
{
	warn("EAPOL %s", why);
	abort_connection();
}

static void pmk_to_ptk()
{
	uint8_t *mac1, *mac2;
	uint8_t *nonce1, *nonce2;

	memcpy(amac, ap.bssid, 6);

	if(memcmp(smac, amac, 6) < 0) {
		mac1 = smac;
		mac2 = amac;
	} else {
		mac1 = amac;
		mac2 = smac;
	}

	if(memcmp(snonce, anonce, 32) < 0) {
		nonce1 = snonce;
		nonce2 = anonce;
	} else {
		nonce1 = anonce;
		nonce2 = snonce;
	}

	uint8_t key[60];

	char* astr = "Pairwise key expansion";
	PRF480(key, PSK, astr, mac1, mac2, nonce1, nonce2);

	memcpy(KCK, key +  0, 16);
	memcpy(KEK, key + 16, 16);
	memcpy(PTK, key + 32, 16);

	memzero(key, sizeof(key));
}

/* Message 3 comes with a bunch of IEs (apparently?), among which
   there should be one with the GTK. The whole thing is really
   messed up, so refer to the standard for clues. KDE structure
   comes directly from 802.11, it's a kind of tagged union there
   but we only need a single case, namely the GTK KDE. */

/* Ref. IEEE 802.11-2012 Table 11-6 */
static const char kde_type_gtk[4] = { 0x00, 0x0F, 0xAC, 0x01 };

static int store_gtk(int idx, byte* buf, int len)
{
	int explen = ap.tkipgroup ? 32 : 16;

	if(len != explen)
		return -1;

	gtkindex = idx;

	memcpy(GTK, buf, 16);

	/* From wpa_supplicant: swap Tx/Rx for Michael MIC.
	   No idea where this comes from, but it's necessary
	   to get the right key. */
	if(ap.tkipgroup) {
		memcpy(GTK + 16, buf + 24, 8);
		memcpy(GTK + 24, buf + 16, 8);
	}

	return 0;
}

static int fetch_gtk(char* buf, int len)
{
	struct kde* kd;
	int kdlen, idx;

	char* ptr = buf;
	char* end = buf + len;

	while(ptr + sizeof(*kd) < end) {
		kd = (struct kde*) ptr;
		kdlen = 2 + kd->len; /* 2 = sizeof(type) + sizeof(len) */
		ptr += kdlen;

		if(ptr > end)
			break;

		int datalen = kd->len + 2 - sizeof(*kd);

		if(kd->magic != 0xDD)
			continue;
		if(memcmp(kd->type, kde_type_gtk, 4))
			continue;
		if(datalen < 2 + 16) /* flags[1] + pad[1] + min key length */
			continue;
		if(!(idx = kd->data[0] & 0x3)) /* key idx is non-zero for GTK */
			return -1;

		byte* key = kd->data + 2;
		int len = datalen - 2;

		return store_gtk(idx, key, len);
	}

	return -1;
}

static void fill_rand(void)
{
	int rlen = sizeof(snonce);
	char rand[rlen];

	long fd, rd;
	char* urandom = "/dev/urandom";

	if((fd = open(urandom, O_RDONLY)) < 0)
		quit("open %s: %m\n", urandom);
	if((rd = read(fd, rand, rlen)) < 0)
		quit("read %s: %m\n", urandom);
	if(rd < rlen)
		quit("read %s\n", urandom);

	close(fd);

	memcpy(snonce, rand, sizeof(snonce));
}

static void cleanup_keys(void)
{
	memzero(packet, sizeof(packet));
	memzero(anonce, sizeof(anonce));
	memzero(snonce, sizeof(snonce));
	memzero(PTK, sizeof(PTK));
	memzero(GTK, sizeof(GTK));
	/* we may need KCK and KEK for GTK rekeying */
}

void reset_eapol_state(void)
{
	cleanup_keys();

	memzero(KCK, sizeof(KCK));
	memzero(GTK, sizeof(GTK));
	memzero(KEK, sizeof(KEK));

	memzero(snonce, sizeof(snonce));
	memzero(anonce, sizeof(anonce));
	memzero(amac, sizeof(amac));

	version = 0;
}

/* The tricky part here. EAPOL packet 1/4 may arrive before the ASSOCIATE msg
   on netlink, but sending may not work until the link is fully associated.
   Packets sent until then get silently dropped somewhere. So at the time we
   get packet 1/4, we may not yet be able to reply.
   
   To get around this, we prime the EAPOL state machine before we send
   ASSOCIATE request and let it receive packet 1/4 early, but only reply
   with 2/4 once netlink reports association.

   Since it all depends on relative timing of unrelated events, we can not
   be sure it always happens like this. We may get 1/4 after ASSOCIATE msg,
   so we must be ready to reply immediately as well. */

void prime_eapol_state(void)
{
	eapolstate = ES_WAITING_1_4;
	eapolsends = 0;
}

void allow_eapol_sends(void)
{
	if(eapolstate == ES_WAITING_1_4)
		eapolsends = 1;
	else
		send_packet_2();
}

static int send_packet(char* buf, int len)
{
	int fd = rawsock;
	struct sockaddr_ll dest;
	long wr;

	memzero(&dest, sizeof(dest));
	dest.sll_family = AF_PACKET;
	dest.sll_protocol = htons(ETH_P_PAE);
	dest.sll_ifindex = ifindex;
	memcpy(dest.sll_addr, amac, 6);

	if((wr = sendto(fd, buf, len, 0, (struct sockaddr*)&dest, sizeof(dest))) < 0)
		warn("send: %m\n");
	else if(wr != len)
		warn("send incomplete\n");
	else
		return 0;

	abort_connection();

	return -1;
}

static int ptype(struct eapolkey* ek, int bits)
{
	int keyinfo = ntohs(ek->keyinfo);
	int keytype = keyinfo & KI_TYPEMASK;
	int mask = KI_PAIRWISE | KI_ACK | KI_SECURE | KI_MIC | KI_ENCRYPTED;

	if(keytype != KI_SHA)
		return 0;
	if((keyinfo & mask) != bits)
		return 0;

	return 1;
}

static void recv_packet_1(struct eapolkey* ek)
{
	/* wpa_supplicant does not check ek->version */

	if(ek->type != EAPOL_KEY_RSN)
		return xabort("packet 1/4 wrong type");
	if(!ptype(ek, KI_PAIRWISE | KI_ACK))
		return ignore("packet 1/4 wrong bits");

	version = ek->version;
	memcpy(anonce, ek->nonce, sizeof(anonce));
	memcpy(replay, ek->replay, sizeof(replay));

	fill_rand();
	pmk_to_ptk();

	if(eapolsends)
		return send_packet_2();
	else
		eapolstate = ES_WAITING_2_4;
}

/* Packet 2/4 must carry IEs, the same ones we've sent already
   with the ASSOCIATE command. The point in doing so is not clear,
   but there are APs (like the Android hotspot) that *do* check them
   and bail out if IEs are not there.

   The fact they match exactly the ASSOCIATE payload may be accidental.
   Really needs a reference here. But they do seem to match in practice. */

static void send_packet_2(void)
{
	struct eapolkey* ek = (struct eapolkey*) packet;

	ek->version = version;
	ek->pactype = EAPOL_KEY;
	ek->type = EAPOL_KEY_RSN;
	ek->keyinfo = htons(KI_SHA | KI_PAIRWISE | KI_MIC);
	ek->keylen = htons(16);
	memcpy(ek->replay, replay, sizeof(replay));
	memcpy(ek->nonce, snonce, sizeof(snonce));
	memzero(ek->iv, sizeof(ek->iv));
	memzero(ek->rsc, sizeof(ek->rsc));
	memzero(ek->mic, sizeof(ek->mic));
	memzero(ek->_reserved, sizeof(ek->_reserved));

	const char* payload = ap.ies;
	int paylen = ap.iesize;
	int paclen = sizeof(*ek) + paylen;

	ek->paylen = htons(paylen);
	ek->paclen = htons(paclen - 4);
	memcpy(ek->payload, payload, paylen);

	make_mic(ek->mic, KCK, packet, paclen);

	if(send_packet(packet, paclen))
		return;

	eapolstate = ES_WAITING_3_4;
}

static void recv_packet_3(struct eapolkey* ek)
{
	char* pacbuf = (char*)ek;
	int paclen = 4 + ntohs(ek->paclen);

	if(ptype(ek, KI_PAIRWISE | KI_ACK))
		return xabort("packet 1/4 resend detected");
	if(!ptype(ek, KI_PAIRWISE | KI_ACK | KI_MIC | KI_ENCRYPTED | KI_SECURE))
		return xabort("packet 3/4 wrong bits");

	if(memcmp(anonce, ek->nonce, sizeof(anonce)))
		return xabort("packet 3/4 nonce changed");
	if(memcmp(replay, ek->replay, sizeof(replay)) >= 0)
		return xabort("packet 3/4 replay fail");
	if(check_mic(ek->mic, KCK, pacbuf, paclen))
		return xabort("packet 3/4 bad MIC");

	char* payload = ek->payload;
	int paylen = ntohs(ek->paylen);

	if(unwrap_key(KEK, payload, paylen))
		return xabort("packet 3/4 cannot unwrap");
	if(fetch_gtk(payload + 8, paylen - 8))
		return xabort("packet 3/4 cannot fetch GTK");

	memcpy(RSC, ek->rsc, 6); /* it's 8 bytes but only 6 are used */
	memcpy(replay, ek->replay, sizeof(replay));

	return send_packet_4();
}

static void send_packet_4(void)
{
	struct eapolkey* ek = (struct eapolkey*) packet;

	ek->version = version;
	ek->pactype = 3;
	ek->type = 2;
	ek->keyinfo = htons(KI_SHA | KI_PAIRWISE | KI_MIC | KI_SECURE);
	ek->keylen = 0;
	memcpy(ek->replay, replay, sizeof(replay));
	memzero(ek->nonce, sizeof(ek->nonce));
	memzero(ek->iv, sizeof(ek->iv));
	memzero(ek->rsc, sizeof(ek->rsc));
	memzero(ek->mic, sizeof(ek->mic));
	memzero(ek->_reserved, sizeof(ek->_reserved));

	int paclen = sizeof(*ek);

	ek->paylen = htons(0);
	ek->paclen = htons(paclen - 4);

	make_mic(ek->mic, KCK, packet, paclen);

	if(send_packet(packet, paclen))
		return;

	eapolstate = ES_NEGOTIATED;

	upload_ptk();
	upload_gtk();
	cleanup_keys();

	handle_connect();
}

/* Group rekey packets may arrive at any time, and they are the only
   reason to keep rawsock open past the initial key negotiations.
   The AP decides when to send them, typically once in N hours.

   Because of the way dispatch() below works, any EAPOL packets
   arriving after packet 4/4 has been sent will be treated as
   group rekey request, and rejected if they don't look like one. */

static void recv_group_1(struct eapolkey* ek)
{
	char* pacbuf = (char*)ek;
	int paclen = 4 + ntohs(ek->paclen);

	if(ek->type != EAPOL_KEY_RSN)
		return ignore("re-keying with a different key type");
	if(!ptype(ek, KI_SECURE | KI_ENCRYPTED | KI_ACK | KI_MIC))
		return ignore("not a rekey request packet");
	if(memcmp(replay, ek->replay, sizeof(replay)) >= 0)
		return ignore("packet 1/2 replay");
	if(check_mic(ek->mic, KCK, pacbuf, paclen))
		return ignore("packet 1/2 bad MIC");

	char* payload = ek->payload;
	int paylen = ntohs(ek->paylen);

	if(unwrap_key(KEK, payload, paylen))
		return xabort("packet 1/2 cannot unwrap");
	if(fetch_gtk(payload + 8, paylen - 8))
		return xabort("packet 1/2 cannot fetch GTK");

	memcpy(RSC, ek->rsc, 6); /* it's 8 bytes but only 6 are used */
	memcpy(replay, ek->replay, sizeof(replay));

	return send_group_2();
}

void send_group_2(void)
{
	struct eapolkey* ek = (struct eapolkey*) packet;

	ek->version = version;
	ek->pactype = 3;
	ek->type = 2;
	ek->keyinfo = htons(KI_MIC | KI_SECURE);
	ek->keylen = 0;
	memcpy(ek->replay, replay, sizeof(replay));
	memzero(ek->nonce, sizeof(snonce));
	memzero(ek->iv, sizeof(ek->iv));
	memzero(ek->rsc, sizeof(ek->rsc));
	memzero(ek->mic, sizeof(ek->mic));
	memzero(ek->_reserved, sizeof(ek->_reserved));

	int paclen = sizeof(*ek);

	ek->paylen = htons(0);
	ek->paclen = htons(paclen);

	make_mic(ek->mic, KCK, packet, paclen);

	if(send_packet(packet, paclen))
		return;

	upload_gtk();
}

static void dispatch(struct eapolkey* ek)
{
	switch(eapolstate) {
		case ES_WAITING_1_4: return recv_packet_1(ek);
		case ES_WAITING_3_4: return recv_packet_3(ek);
		case ES_NEGOTIATED: return recv_group_1(ek);
		default: return ignore("unexpected packet");
	}
}

void handle_rawsock(void)
{
	struct sockaddr_ll sender;
	int psize = sizeof(packet);
	unsigned asize = sizeof(sender);
	int fd = rawsock;
	int rd;

	if((rd = recvfrom(fd, packet, psize, 0, (struct sockaddr*)&sender, &asize)) < 0)
		return warn("EAPOL: %m\n");

	if(memcmp(ap.bssid, sender.sll_addr, 6))
		return warn("EAPOL stray packet\n");

	struct eapolkey* ek = (struct eapolkey*) packet;
	int eksize = sizeof(*ek);

	if(rd < eksize)
		return ignore("packet too short");
	if(ntohs(ek->paclen) + 4 != rd)
		return ignore("packet size mismatch");
	if(eksize + ntohs(ek->paylen) > rd)
		return ignore("truncated payload");
	if(ek->pactype != EAPOL_KEY)
		return ignore("not a KEY packet");

	return dispatch(ek);
}
