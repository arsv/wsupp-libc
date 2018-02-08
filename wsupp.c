#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>


#include "common.h"
#include "wsupp.h"

const char errtag[] = "wifi";

static sigset_t defsigset;
static struct pollfd pfds[3+NCONNS];
static int npfds;
static struct timespec pollts;
static int timerset;

int opermode;
int pollset;
int sigterm;
int done;

static void sighandler(int sig)
{
	switch(sig) {
		case SIGCHLD:
			reap_dhcp();
			break;
		case SIGINT:
		case SIGTERM: sigterm = 1;
	}
}

static void xsigaction(int sig, struct sigaction* sa)
{
	if(sigaction(sig, sa, NULL) < 0)
		fail("sigaction: %m\n");
}

static void setup_signals(void)
{
	struct sigaction sa = {
		.sa_handler = sighandler,
		.sa_flags = 0
	};

	sigaddset(&sa.sa_mask, SIGINT);
	sigaddset(&sa.sa_mask, SIGTERM);
	sigaddset(&sa.sa_mask, SIGHUP);
	sigaddset(&sa.sa_mask, SIGALRM);
	sigaddset(&sa.sa_mask, SIGCHLD);

	xsigaction(SIGINT,  &sa);
	xsigaction(SIGTERM, &sa);
	xsigaction(SIGHUP,  &sa);
	xsigaction(SIGALRM, &sa);
	xsigaction(SIGCHLD, &sa);

	sa.sa_handler = SIG_IGN;

	xsigaction(SIGPIPE, &sa);
}

static void set_pollfd(struct pollfd* pfd, int fd)
{
	if(fd > 0) {
		pfd->fd = fd;
		pfd->events = POLLIN;
	} else {
		pfd->fd = -1;
		pfd->events = 0;
	}
}

static void close_conn(struct conn* cn)
{
	close(cn->fd);
	memzero(cn, sizeof(*cn));
	pollset = 0;
}

static void check_conn(struct pollfd* pf, struct conn* cn)
{
	if(pf->revents & POLLIN)
		handle_conn(cn);
	if(pf->revents & ~POLLIN)
		close_conn(cn);
}

static void check_netlink(struct pollfd* pf)
{
	if(pf->revents & POLLIN)
		handle_netlink();
	if(pf->revents & ~POLLIN)
		quit("lost netlink connection\n");
}

static void check_control(struct pollfd* pf)
{
	if(pf->revents & POLLIN)
		handle_control();
	if(pf->revents & ~POLLIN)
		quit("lost control socket\n");

	pollset = 0;
}

static void check_rawsock(struct pollfd* pf)
{
	if(pf->revents & POLLIN)
		handle_rawsock();
	if(!(pf->revents & ~POLLIN))
		return;

	close(rawsock);
	rawsock = -1;
	pf->fd = -1;
}

static void check_rfkill(struct pollfd* pf)
{
	if(pf->revents & POLLIN)
		handle_rfkill();
	if(!(pf->revents & ~POLLIN))
		return;

	close(rfkill);
	rfkill = -1;
	pf->fd = -1;
}

static void update_pollfds(void)
{
	set_pollfd(&pfds[0], netlink);
	set_pollfd(&pfds[1], rawsock);
	set_pollfd(&pfds[2], ctrlfd);
	set_pollfd(&pfds[3], rfkill);

	int i, n = 4;

	for(i = 0; i < nconns; i++)
		set_pollfd(&pfds[n+i], conns[i].fd);

	npfds = n + nconns;
	pollset = 1;
}

static void check_polled_fds(void)
{
	int i, n = 4;

	for(i = 0; i < nconns; i++)
		check_conn(&pfds[n+i], &conns[i]);

	check_netlink(&pfds[0]);
	check_rawsock(&pfds[1]);
	check_control(&pfds[2]);
	check_rfkill(&pfds[3]);
}

void clr_timer(void)
{
	pollts.tv_sec = 0;
	pollts.tv_nsec = 0;
	timerset = 0;
}

void set_timer(int seconds)
{
	pollts.tv_sec = seconds;
	pollts.tv_nsec = 0;
	timerset = 1;
}

static void timer_expired(void)
{
	clr_timer();

	if(authstate == AS_NETDOWN) {
		if(!rfkilled)
			opermode = OP_EXIT;
		else
			authstate = AS_IDLE;
		return;
	}

	if(authstate == AS_CONNECTED)
		routine_bg_scan();
	else if(authstate != AS_IDLE)
		abort_connection();
	else
		routine_fg_scan();
}

static void xshutdown(void)
{
	sigterm = 0;

	switch(opermode) {
		case OP_EXIT:
		case OP_EXITREQ:
			quit("second exit request\n");
	}
	switch(authstate) {
		case AS_IDLE:
		case AS_NETDOWN:
			opermode = OP_EXIT;
			return;
	}

	if(start_disconnect() < 0)
		opermode = OP_EXIT;
	else
		opermode = OP_EXITREQ;
}

int main(int argc, char** argv, char** envp)
{
	int i = 1, ret;
	char* name;

	if(i < argc)
		name = argv[i++];
	else
		fail("too few arguments\n");
	if(i < argc)
		fail("too many arguments\n");

	setup_signals();
	setup_netlink();
	setup_iface(name);
	setup_control();
	retry_rfkill();

	opermode = OP_NEUTRAL;
	load_state();
	routine_fg_scan();

	while(opermode) {
		struct timespec* ts = timerset ? &pollts : NULL;

		if(!pollset)
			update_pollfds();
		if((ret = ppoll(pfds, npfds, ts, &defsigset)) > 0)
			check_polled_fds();
		else if(ret == 0)
			timer_expired();
		else if(errno != EINTR)
			quit("ppoll: %m\n");
		if(sigterm)
			xshutdown();

		save_config();
	}

	save_state();
	unlink_control();

	return 0;
}
