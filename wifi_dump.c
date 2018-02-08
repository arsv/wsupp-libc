#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "nlusctl.h"
#include "control.h"
#include "wifi.h"

typedef int (*qcmp2)(const void* a, const void* b);

/* Wi-Fi channel designation.
   
   Ref. https://en.wikipedia.org/wiki/List_of_WLAN_channels
 
   Bands a and b refer to 802.11a and 802.11b respectively. */

static int inrange(int freq, int a, int b, int s, int i)
{
	if(freq < a)
		return 0;
	if(freq > b)
		return 0;

	int d = freq - a;

	if(d % s)
		return 0;

	return i + (d/s);
}

void get_channel(int freq, int* chan, char* band)
{
	int s;

	if(freq == 2484) {
		*chan = 14;
		*band = 'b';
	} else if((s = inrange(freq, 2412, 2472, 5, 1))) {
		*chan = s;
		*band = 'b';
	} else if((s = inrange(freq, 5035, 5865, 5, 7))) {
		*chan = s;
		*band = 'a';
	} else if((s = inrange(freq, 4915, 4980, 5, 183))) {
		*chan = s;
		*band = 'a';
	} else {
		*chan = 0;
		*band = '\0';
	}
}

#define DICTEND -1

static const struct dict {
	int val;
	char name[16];
} wistates[] = {
	{ WS_IDLE,       "Idle"       },
	{ WS_RFKILLED,   "RF-kill"    },
	{ WS_NETDOWN,    "Net down"   },
	{ WS_EXTERNAL,   "External"   },
	{ WS_SCANNING,   "Scanning"   },
	{ WS_CONNECTING, "Connecting" },
	{ WS_CONNECTED,  "Connected"  },
	{ DICTEND,       ""           }
};

static int cmp_int(attr at, attr bt, int key)
{
	int* na = uc_sub_int(at, key);
	int* nb = uc_sub_int(bt, key);

	if(!na && nb)
		return -1;
	if(na && !nb)
		return  1;
	if(!na || !nb)
		return 0;
	if(*na < *nb)
		return -1;
	if(*na > *nb)
		return  1;

	return 0;
}

static int scan_ord(const void* a, const void* b)
{
	attr at = *((attr*)a);
	attr bt = *((attr*)b);
	int ret;

	if((ret = cmp_int(at, bt, ATTR_SIGNAL)))
		return -ret;
	if((ret = cmp_int(at, bt, ATTR_FREQ)))
		return ret;

	return 0;
}

static void get_int(MSG, int attr, int* val)
{
	int* p;

	if((p = uc_get_int(msg, attr)))
		*val = *p;
	else
		*val = 0;
}

static int printable(unsigned char c)
{
	return (c >= 0x20);
}

static void print_ssid(attr at)
{
	if(!at) return;

	int i, len = uc_paylen(at);
	byte* ssid = uc_payload(at);

	for(i = 0; i < len; i++)
		if(printable(ssid[i]))
			printf("%c", ssid[i]);
		else
			printf("\\x%02X", ssid[i] & 0xFF);
}

static void print_mac(attr at)
{
	if(!at || uc_paylen(at) != 6)
		return;

	byte* b = uc_payload(at);

	printf("%02X:%02X:%02X:%02X:%02X:%02X",
		b[0], b[1], b[2], b[3], b[4], b[5]);
}

void print_station(CTX, MSG)
{
	attr ssid = uc_get(msg, ATTR_SSID);
	attr bssid = uc_get(msg, ATTR_BSSID);
	int freq, chan;
	char band;

	get_int(msg, ATTR_FREQ, &freq);
	get_channel(freq, &chan, &band);

	print_ssid(ssid);

	if(ctx->showbss) {
		printf(" ");
		print_mac(bssid);
	}

	if(!freq)
		;
	else if(band)
		printf(" (%i%c/%iMHz)", chan, band, freq);
	else
		printf(" (%iMHz)", freq);
}

static void sub_int(AT, int attr, int* val)
{
	int* p;

	if((p = uc_sub_int(at, attr)))
		*val = *p;
	else
		*val = 0;
}

static void print_scanline(CTX, AT)
{
	attr ssid = uc_sub(at, ATTR_SSID);
	attr bssid = uc_sub(at, ATTR_BSSID);
	attr prio = uc_sub(at, ATTR_PRIO);
	int signal, freq, chan;
	char band;

	if(!bssid || !ssid) return;

	sub_int(at, ATTR_SIGNAL, &signal);
	sub_int(at, ATTR_FREQ, &freq);

	get_channel(freq, &chan, &band);

	printf("AP %i ", signal/100);

	if(band)
		printf("%3i%c", chan, band);
	else
		printf("%4i", freq);

	if(ctx->showbss) {
		printf("  ");
		print_mac(bssid);
	}

	printf("  ");
	print_ssid(ssid);

	if(prio) printf(" *");

	printf("\n");
}

static attr* prep_list(CTX, MSG, int key, qcmp2 cmp)
{
	int n = 0, i = 0;
	attr at;

	for(at = uc_get_0(msg); at; at = uc_get_n(msg, at))
		if(at->key == key)
			n++;

	attr* refs = malloc((n+1)*sizeof(void*));

	for(at = uc_get_0(msg); at && i < n; at = uc_get_n(msg, at))
		if(at->key == key)
			refs[i++] = at;
	refs[i] = NULL;

	qsort(refs, i, sizeof(void*), cmp);

	return refs;
}

static void print_scan_results(CTX, MSG, int nl)
{
	attr* scans = prep_list(ctx, msg, ATTR_SCAN, scan_ord);

	for(attr* ap = scans; *ap; ap++)
		print_scanline(ctx, *ap);

	if(nl && *scans) printf("\n");
}

static void print_status(CTX, MSG)
{
	int state;
	get_int(msg, ATTR_STATE, &state);

	const struct dict* dc = wistates;
	const struct dict* kv;

	for(kv = dc; kv->val != DICTEND; kv++)
		if(kv->val == state)
			break;
	if(kv->val == DICTEND)
		printf("??");
	else
		printf("%s", kv->name);

	printf(" AP ");
	print_station(ctx, msg);
	printf("\n");
}

void dump_scanlist(CTX, MSG)
{
	print_scan_results(ctx, msg, 0);
}

void dump_status(CTX, MSG)
{
	print_scan_results(ctx, msg, 1);
	print_status(ctx, msg);
}

void warn_sta(CTX, char* text, MSG)
{
	printf("%s AP ", text);
	print_station(ctx, msg);
	printf("\n");
}
