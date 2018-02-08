#include <sys/socket.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/un.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "common.h"
#include "control.h"
#include "nlusctl.h"

#define PAGE 4096

#include "common.h"
#include "wsupp.h"

/* Userspace control socket code, accepts and handles commands
   from the frontend tool (wifi). */

int ctrlfd;

static char rxbuf[100];
static char txbuf[100];
struct ucbuf uc;

struct heap {
	void* brk;
	void* ptr;
	void* end;
};

#define CN struct conn* cn __unused
#define MSG struct ucmsg* msg __unused

#define REPLIED 1

static void send_report(char* buf, int len)
{
	struct conn* cn;
	int fd;

	for(cn = conns; cn < conns + nconns; cn++) {
		if(!cn->rep || (fd = cn->fd) <= 0)
			continue;

		struct itimerval old, itv = {
			.it_interval = { 0, 0 },
			.it_value = { 1, 0 }
		};

		setitimer(ITIMER_REAL, &itv, &old);

		if(write(fd, buf, len) < 0)
			shutdown(fd, SHUT_RDWR);

		setitimer(ITIMER_REAL, &old, NULL);
	}
}

static void report_simple(int cmd)
{
	char buf[64];
	struct ucbuf uc = {
		.brk = buf,
		.ptr = buf,
		.end = buf + sizeof(buf)
	};

	uc_put_hdr(&uc, cmd);
	uc_put_end(&uc);

	send_report(uc.brk, uc.ptr - uc.brk);
}

void report_net_down(void)
{
	report_simple(REP_WI_NET_DOWN);
}

void report_scanning(void)
{
	report_simple(REP_WI_SCANNING);
}

void report_scan_done(void)
{
	report_simple(REP_WI_SCAN_DONE);
}

void report_scan_fail(void)
{
	report_simple(REP_WI_SCAN_FAIL);
}

void report_no_connect(void)
{
	report_simple(REP_WI_NO_CONNECT);
}

static void report_station(int cmd)
{
	char buf[256];
	struct ucbuf uc = {
		.brk = buf,
		.ptr = buf,
		.end = buf + sizeof(buf)
	};

	uc_put_hdr(&uc, cmd);
	uc_put_bin(&uc, ATTR_BSSID, ap.bssid, sizeof(ap.bssid));
	uc_put_bin(&uc, ATTR_SSID, ap.ssid, ap.slen);
	uc_put_int(&uc, ATTR_FREQ, ap.freq);
	uc_put_end(&uc);

	send_report(uc.brk, uc.ptr - uc.brk);
}

void report_disconnect(void)
{
	report_station(REP_WI_DISCONNECT);
}

void report_connected(void)
{
	report_station(REP_WI_CONNECTED);
}

static void start_reply(int cmd)
{
	void* buf = NULL;
	int len;

	buf = txbuf;
	len = sizeof(txbuf);

	uc.brk = buf;
	uc.ptr = buf;
	uc.end = buf + len;

	uc_put_hdr(&uc, cmd);
}

static int send_reply(CN)
{
	uc_put_end(&uc);

	writeall(cn->fd, uc.brk, uc.ptr - uc.brk);

	return REPLIED;
}

static int reply(CN, int err)
{
	start_reply(err);

	return send_reply(cn);
}

static int estimate_status(void)
{
	int scansp = nscans*(sizeof(struct scan) + 10*sizeof(struct ucattr));

	return scansp + 128;
}

static void prep_heap(struct heap* hp, int size)
{
	size += (PAGE - size % PAGE) % PAGE;

	void* buf = malloc(size);

	hp->brk = buf;
	hp->ptr = buf;
	hp->end = buf + size;
}

static void free_heap(struct heap* hp)
{
	free(hp->brk);
	hp->brk = NULL;
	hp->ptr = NULL;
	hp->end = NULL;
}

static int common_wifi_state(void)
{
	if(authstate == AS_CONNECTED)
		return WS_CONNECTED;
	if(authstate == AS_NETDOWN)
		return rfkilled ? WS_RFKILLED : WS_NETDOWN;
	if(authstate == AS_EXTERNAL)
		return WS_EXTERNAL;
	if(authstate != AS_IDLE)
		return WS_CONNECTING;
	if(scanstate != SS_IDLE)
		return WS_SCANNING;

	return WS_IDLE;
}

static void put_status_wifi(struct ucbuf* uc)
{
	uc_put_int(uc, ATTR_IFI, ifindex);
	uc_put_str(uc, ATTR_NAME, ifname);
	uc_put_int(uc, ATTR_STATE, common_wifi_state());

	if(authstate != AS_IDLE || ap.fixed)
		uc_put_bin(uc, ATTR_SSID, ap.ssid, ap.slen);
	if(authstate != AS_IDLE) {
		uc_put_bin(uc, ATTR_BSSID, ap.bssid, sizeof(ap.bssid));
		uc_put_int(uc, ATTR_FREQ, ap.freq);
	}
}

static void put_status_scans(struct ucbuf* uc)
{
	struct scan* sc;
	struct ucattr* nn;

	for(sc = scans; sc < scans + nscans; sc++) {
		if(!sc->freq) continue;
		nn = uc_put_nest(uc, ATTR_SCAN);
		uc_put_int(uc, ATTR_FREQ,   sc->freq);
		uc_put_int(uc, ATTR_TYPE,   sc->type);
		uc_put_int(uc, ATTR_SIGNAL, sc->signal);
		uc_put_bin(uc, ATTR_BSSID,  sc->bssid, sizeof(sc->bssid));
		uc_put_bin(uc, ATTR_SSID,   sc->ssid, sc->slen);

		if(!(sc->flags & SF_PASS))
			;
		else if(!(sc->flags & SF_GOOD))
			;
		else uc_put_flag(uc, ATTR_PRIO);

		uc_end_nest(uc, nn);
	}
}

static int cmd_status(CN, MSG)
{
	struct heap hp;

	prep_heap(&hp, estimate_status());

	uc_buf_set(&uc, hp.brk, hp.end - hp.brk);
	uc_put_hdr(&uc, 0);
	put_status_wifi(&uc);
	put_status_scans(&uc);
	uc_put_end(&uc);

	int ret = send_reply(cn);

	free_heap(&hp);

	cn->rep = 0;

	return ret;
}

static int cmd_device(CN, MSG)
{
	char buf[64];

	uc_buf_set(&uc, buf, sizeof(buf));
	uc_put_hdr(&uc, 0);
	uc_put_int(&uc, ATTR_IFI, ifindex);
	uc_put_str(&uc, ATTR_NAME, ifname);
	uc_put_end(&uc);

	return send_reply(cn);
}

static int cmd_scan(CN, MSG)
{
	int ret;

	if((ret = start_void_scan()) < 0)
		return ret;

	cn->rep = 1;

	return 0;
}

static int cmd_neutral(CN, MSG)
{
	int ret;

	opermode = OP_NEUTRAL;

	if((ret = start_disconnect()) < 0)
		return ret;

	cn->rep = 1;

	clr_timer();

	return 0;
}

static int configure_station(MSG)
{
	struct ucattr* assid;
	struct ucattr* apsk;

	reset_station();

	if(!(assid = uc_get(msg, ATTR_SSID)))
		return 0;

	byte* ssid = uc_payload(assid);
	int slen = uc_paylen(assid);

	if(!(apsk = uc_get(msg, ATTR_PSK)))
		return set_fixed_saved(ssid, slen);
	else if(uc_paylen(apsk) == 32)
		return set_fixed_given(ssid, slen, uc_payload(apsk));
	else {
		warn("invalid PSK length %i\n", uc_paylen(apsk));
		return -EINVAL;
	}
}

/* ACK to the command should preceed any notifications caused by the command.
   Since reassess_wifi_situation() is pretty long and involved piece of code,
   we reply early to make sure possible messages generated while starting
   connection do not confuse the client.

   Note at this point reassess_wifi_situation() does *not* generate any
   notifications. Not even SCANNING, that one is issued reactively on
   NL80211_CMD_TRIGGER_SCAN. */

static int cmd_connect(CN, MSG)
{
	int ret;

	if(authstate != AS_IDLE)
		return -EBUSY;
	if(scanstate != SS_IDLE)
		return -EBUSY;

	if((ret = configure_station(msg)) < 0)
		return ret;

	opermode = OP_ONESHOT;

	cn->rep = 1;

	ret = reply(cn, 0);

	clr_timer();

	reassess_wifi_situation();

	return ret;
}

static int cmd_forget(CN, MSG)
{
	int ret;
	struct ucattr* at;
	struct scan* sc;

	if(!(at = uc_get(msg, ATTR_SSID)))
		return -EINVAL;

	byte* ssid = uc_payload(at);
	int slen = uc_paylen(at);

	if((ret = drop_psk(ssid, slen)) < 0)
		return ret;

	for(sc = scans; sc < scans + nscans; sc++)
		if(sc->slen != slen)
			;
		else if(memcmp(sc->ssid, ssid, slen))
			;
		else sc->flags &= ~SF_PASS;

	return 0;
}

static const struct cmd {
	int cmd;
	int (*call)(CN, MSG);
} commands[] = {
	{ CMD_WI_STATUS,  cmd_status  },
	{ CMD_WI_DEVICE,  cmd_device  },
	{ CMD_WI_SCAN,    cmd_scan    },
	{ CMD_WI_NEUTRAL, cmd_neutral },
	{ CMD_WI_CONNECT, cmd_connect },
	{ CMD_WI_FORGET,  cmd_forget  }
};

static int dispatch_cmd(CN, MSG)
{
	const struct cmd* cd;
	int cmd = msg->cmd;
	int ret;

	for(cd = commands; cd < commands + ARRAY_SIZE(commands); cd++)
		if(cd->cmd != cmd)
			continue;
		else if((ret = cd->call(cn, msg)) > 0)
			return ret;
		else
			return reply(cn, ret);

	return reply(cn, -ENOSYS);
}

static void shutdown_conn(struct conn* cn)
{
	shutdown(cn->fd, SHUT_RDWR);
}

void handle_conn(struct conn* cn)
{
	int ret, fd = cn->fd;

	struct urbuf ur = {
		.buf = rxbuf,
		.mptr = rxbuf,
		.rptr = rxbuf,
		.end = rxbuf + sizeof(rxbuf)
	};
	struct itimerval old, itv = {
		.it_interval = { 0, 0 },
		.it_value = { 1, 0 }
	};

	setitimer(0, &itv, &old);

	while(1) {
		if((ret = uc_recv(fd, &ur, 0)) < 0)
			break;
		if((ret = dispatch_cmd(cn, ur.msg)) < 0)
			break;
	}

	if(ret < 0 && ret != -EBADF && ret != -EAGAIN)
		shutdown_conn(cn);

	setitimer(0, &old, NULL);
}

void setup_control(void)
{
	const int flags = SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC;
	struct sockaddr_un addr = {
		.sun_family = AF_UNIX,
		.sun_path = WICTL
	};
	int fd;

	if((fd = socket(AF_UNIX, flags, 0)) < 0)
		fail("socket AF_UNIX: %m\n");

	if(bind(fd, (void*)&addr, sizeof(addr)) < 0)
		fail("bind %s: %m\n", addr.sun_path);
	if(listen(fd, 1) < 0)
		quit("listen %s: %m", addr.sun_path);

	ctrlfd = fd;
}

void unlink_control(void)
{
	unlink(WICTL);
}

void handle_control(void)
{
	int cfd, sfd = ctrlfd;
	struct sockaddr addr;
	unsigned addr_len = sizeof(addr);
	struct conn *cn;

	while((cfd = accept(sfd, &addr, &addr_len)) > 0)
		if((cn = grab_conn_slot()))
			cn->fd = cfd;
		else
			close(cfd);
}
