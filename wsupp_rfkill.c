#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "common.h"
#include "rfkill.h"
#include "wsupp.h"

/* When a card gets rf-killed, the link loses IFF_UP and RTNL gets notification
   of a state change. But when rfkill gets undone, the reverse does not happen.
   The interface remains in "down" state and must be commanded back "up".
   RTNL layer also gets no notifications of any kind that rf-unkill happened
   (and this tool doesn't even listen to RTNL notifications).

   The only somewhat reliable way to be notified is by listening to /dev/rfkill.
   But rfkill device is provided by a standalone module that may not be loadeded
   at any given time, and may get un-/re-loaded. Normally this does not happens,
   so wimon keeps the fd open. However if open attempt fails, wsupp will try to
   re-open it on any suitable occasion. This may lead to redundant open calls
   in case rfkill is in fact missing, but there's probably no other way around
   this. Hopefully events that trigger reopen attempts are rare.

   Another problem is that /dev/rfkill reports events for rfkill devices (idx
   in the struct below) which do *not* match netdev ifi-s. The trick used here
   is to check for /sys/class/net/$ifname/phy80211/rfkill$idx. The $idx-$ifname
   association seems to be stable for at least as long as the fd remains open,
   but there are no guarantees beyond that.

   The end result of this all is effectively `ifconfig (iface) up` done each
   time some managed link gets un-killed, followed by attempt to re-establish
   connection. */

int rfkill;
int rfkilled;
static int rfkidx;

/* The interface gets brought up with simple ioctls. It could have been done
   with RTNL as well, but setting up RTNL for this mere reason hardly makes
   sense.

   Current code works on interface named $ifname, in contrast with GENL code
   that uses $ifindex instead. Renaming the interface while wsupp runs *will*
   confuse it. */

//#define IFF_UP (1<<0)

static void bring_iface_up(void)
{
	int fd = netlink;
	char* name = ifname;
	uint nlen = strlen(name);
	struct ifreq ifr;
	int ret;

	if(nlen > sizeof(ifr.ifr_name))
		quit("iface name too long: %s\n", name);

	memzero(&ifr, sizeof(ifr));
	memcpy(ifr.ifr_name, name, nlen);

	if((ret = ioctl(fd, SIOCGIFFLAGS, &ifr)) < 0)
		quit("ioctl SIOCGIFFLAGS %s: %m\n", name);

	if(ifr.ifr_flags & IFF_UP)
		return;

	ifr.ifr_flags |= IFF_UP;

	if((ret = ioctl(fd, SIOCSIFFLAGS, &ifr)) < 0)
		quit("ioctl SIOCSIFFLAGS %s: %m\n", name);
}

static int match_rfkill(int idx)
{
	struct stat st;
	int plen = 100;
	char path[plen];

	snprintf(path, plen, "/sys/class/net/%s/phy80211/rfkill", ifname);

	return (stat(path, &st) >= 0);
}

static void handle_event(struct rfkill_event* re)
{
	if(rfkidx < 0) {
		if(match_rfkill(re->idx))
			rfkidx = re->idx;
		else
			return;
	} else if(re->idx != rfkidx) {
		return;
	}

	if(re->soft || re->hard) {
		rfkilled = 1;
		clr_timer();
	} else {
		rfkilled = 0;
		bring_iface_up();
		handle_rfrestored();
	}
}

void retry_rfkill(void)
{
	if(rfkill > 0)
		return;

	rfkill = open("/dev/rfkill", O_RDONLY | O_NONBLOCK);

	rfkidx = -1;
	pollset = 0;
}

/* One event per read() here, even if more are queued. */

void handle_rfkill(void)
{
	char buf[128];
	struct rfkill_event* re;
	int fd = rfkill;
	int rd;

	while((rd = read(fd, buf, sizeof(buf))) > 0) {
		re = (struct rfkill_event*) buf;

		if((ulong)rd < sizeof(*re))
			continue;
		if(re->type != RFKILL_TYPE_WLAN)
			continue;

		handle_event(re);
	}
}
