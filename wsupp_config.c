#define _GNU_SOURCE
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include "common.h"
#include "control.h"
#include "wsupp.h"

/* Mini text editor for the config file. The config looks something like this:

	001122...EEFF Blackhole
	91234A...47AC publicnet
	F419BE...01F5 someothernet

   and wsupp only uses it to store PSKs at this point.

   The data gets read into memory on demand, queried, modified in memory
   if necessary, and synced back to disk. */

#define PAGE 4096
#define MAX_CONFIG_SIZE 64*1024

static char* config;
static int blocklen;
static int datalen;
static int modified;

void save_config(void)
{
	int fd;

	if(!config)
		return;
	if(!modified)
		return;

	if((fd = open(WICFG, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0) {
		warn("cannot open %s: %m", WICFG);
		return;
	}

	writeall(fd, config, datalen);
	close(fd);

	modified = 0;
}

void drop_config(void)
{
	if(!config)
		return;
	if(modified)
		save_config();

	munmap(config, blocklen);
	config = NULL;
	blocklen = 0;
	datalen = 0;
}

static int open_stat_config(int* size)
{
	int fd, ret;
	struct stat st;

	if((fd = open(WICFG, O_RDONLY)) < 0) {
		*size = 0;
		return fd;
	}

	if((ret = fstat(fd, &st)) < 0)
		goto out;
	if(st.st_size > MAX_CONFIG_SIZE) {
		errno = E2BIG;
		goto out;
	}

	*size = st.st_size;
	return fd;
out:
	if(errno != ENOENT)
		warn("%s: %m\n", WICFG);

	close(fd);

	return ret;
}

static int mmap_config_buf(int filesize)
{
	int size = filesize + 1024;
	int prot = PROT_READ | PROT_WRITE;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS;

	size += (PAGE - size % PAGE) % PAGE;

	void* ptr = mmap(NULL, size, prot, flags, -1, 0);

	if(ptr == MAP_FAILED)
		return -1;

	config = ptr;
	blocklen = size;
	datalen = 0;
	modified = 0;

	return 0;
}

static int read_config_whole(int fd, int filesize)
{
	int ret;

	if(filesize <= 0)
		return 0;
	if((ret = read(fd, config, filesize)) < filesize)
		return ret;

	return 0;
}

static int remap_config(int len)
{
	int lenaligned = len + (PAGE - len % PAGE) % PAGE;
	int newblocklen = blocklen + lenaligned;

	void* ptr = mremap(config, blocklen, newblocklen, MREMAP_MAYMOVE);

	if(ptr == MAP_FAILED)
		return -1;

	config = ptr;
	blocklen = len;

	return 0;
}

static int extend_config(int len)
{
	int ret, newdl = datalen + len;

	if(newdl < 0)
		return -1;

	if(newdl > blocklen)
		if((ret = remap_config(newdl)) < 0)
			return ret;

	datalen = newdl;

	return 0;
}

int load_config(void)
{
	int fd;
	long ret;
	int filesize;

	if(config)
		return 0;

	if((fd = open_stat_config(&filesize)) < 0)
		filesize = 0;
	if((ret = mmap_config_buf(filesize)) < 0)
		goto out;
	if((ret = read_config_whole(fd, filesize)) < 0)
		goto out;

	datalen = filesize;
	ret = 0;
out:	
	if(fd >= 0)
		close(fd);
	if(ret < 0 && errno != ENOENT)
		warn("%s: %m\n", WICFG);

	return ret;
}

static int isspace(int c)
{
	return (c == ' ' || c == '\t' || c == '\n');
}

static char* skipline(char* p, char* e)
{
	while(p < e && *p != '\n')
		p++;
	return p;
}

static int setline(struct line* ln, char* p)
{
	char* confend = config + datalen;

	if(p >= confend)
		p = NULL;
	else if(p < config)
		p = NULL;

	ln->start = p ? p : NULL;
	ln->end = p ? skipline(p, confend) : NULL;

	return !!p;
}

static int firstline(struct line* ln)
{
	return setline(ln, config);
}

static int nextline(struct line* ln)
{
	return setline(ln, ln->end + 1);
}

static char* skiparg(char* p, char* e)
{
	for(; p < e && !isspace(*p); p++)
		if(*p == '\\') p++;
	return p;
}

static char* skipsep(char* p, char* e)
{
	while(p < e && isspace(*p))
		p++;
	return p;
}

static int split_line(struct line* ln, struct chunk* ck, int nc)
{
	char* end = ln->end;
	char* p = ln->start;
	int i = 0;

	p = skipsep(p, end);

	while(p < end && i < nc) {
		struct chunk* ci = &ck[i++];
		ci->start = p;
		p = skiparg(p, end);
		ci->end = p;
		p = skipsep(p, end);
	}

	return i;
}

static int chunklen(struct chunk* ck)
{
	return ck->end - ck->start;
}

static int chunkeq(struct chunk* ck, const char* str, int len)
{
	if(chunklen(ck) != len)
		return 0;
	return !memcmp(ck->start, str, len);
}

int find_line(struct line* ln, int i, char* val, int len)
{
	int lk;
	int n = i + 1;
	struct chunk ck[n];

	for(lk = firstline(ln); lk; lk = nextline(ln)) {
		if(split_line(ln, ck, n) < n)
			continue;
		if(!chunkeq(&ck[i], val, len))
			continue;
		return 0;
	}

	return -1;
}

static char* fmt_ssid(char* p, char* e, byte* ssid, int slen)
{
	int i;

	for(i = 0; i < slen; i++) {
		if(p + 5 >= e)
			break;
		if(ssid[i] == '\\') {
			*p++ = '\\';
			*p++ = '\\';
		} else if(ssid[i] == ' ') {
			*p++ = '\\';
			*p++ = ' ';
		} else if(ssid[i] <= 0x20) {
			*p++ = '\\';
			*p++ = 'x';
			snprintf(p, 3, "%02X", ssid[i]);
			p += 2;
		} else {
			*p++ = ssid[i];
		}
	}

	return p;
}

static int find_ssid(struct line* ln, byte* ssid, int slen)
{
	char buf[3*32+5];
	char* p = buf;
	char* e = buf + sizeof(buf) - 1;

	p = fmt_ssid(p, e, ssid, slen);

	/* 0 is psk, 1 is ssid */
	return find_line(ln, 1, buf, p - buf);
}

static void change_part(char* start, char* end, char* buf, int len)
{
	long offs = start - config;
	long offe = end - config;

	int oldlen = offe - offs;
	int newlen = len;

	int shift = newlen - oldlen;
	int shlen = config + datalen - end;

	if(extend_config(shift))
		return;

	char* head = config + offs;
	char* tail = config + offe;

	memmove(tail + shift, tail, shlen);
	memcpy(head, buf, len);
}

static void insert_line(char* buf, int len)
{
	char* at = config + datalen;

	if(extend_config(len + 1))
		return;

	memcpy(at, buf, len);
	at[len] = '\n';
}

static void drop_line(struct line* ln)
{
	if(!ln->start)
		return;

	int shift = -(ln->end - ln->start + 1);
	int shlen = config + datalen - ln->end - 1;

	if(extend_config(shift))
		return;

	memmove(ln->start, ln->end + 1, shlen);

	ln->start = NULL;
	ln->end = NULL;

	modified = 1;
}

void save_line(struct line* ln, char* buf, int len)
{
	if(ln->start)
		change_part(ln->start, ln->end, buf, len);
	else
		insert_line(buf, len);

	ln->start = NULL;
	ln->end = NULL;

	modified = 1;
}

int got_psk_for(byte* ssid, int slen)
{
	struct line ln;

	if(load_config())
		return 0;
	if(find_ssid(&ln, ssid, slen))
		return 0;

	return 1;
}

static const char digits[] = "0123456789ABCDEF";

static int parse_bytes(char* str, int slen, byte* dst, int dlen)
{
	char* p = str;
	char *hh, *ll;
	int i;

	if(slen != 2*dlen)
		return -EINVAL;

	for(i = 0; i < dlen; i++) {
		if(!(hh = strchr(digits, *p++)))
			return -EINVAL;
		if(!(ll = strchr(digits, *p++)))
			return -EINVAL;

		dst[i] = (hh - digits)*16 + (ll - digits);
	}

	return 0;
}

int load_psk(byte* ssid, int slen, byte psk[32])
{
	struct line ln;
	struct chunk ck[2];
	int ret = -ENOKEY;

	if(load_config())
		return ret;
	if(find_ssid(&ln, ssid, slen))
		return ret;
	if(split_line(&ln, ck, 2) < 2)
		return ret;

	struct chunk* cpsk = &ck[0];
	int clen = chunklen(cpsk);

	return parse_bytes(cpsk->start, clen, psk, 32);
}

static char* fmt_bytes(char* p, char* e, byte* data, unsigned len)
{
	unsigned i;

	for(i = 0; i < len; i++) {
		byte di = data[i];
		char hh = digits[(di >> 4) % 16];
		byte ll = digits[di % 16];

		if(p < e) *p++ = hh;
		if(p < e) *p++ = ll;
	}

	return p;
}

void save_psk(byte* ssid, int slen, byte psk[32])
{
	struct line ln;

	char buf[100];
	char* p = buf;
	char* e = buf + sizeof(buf) - 1;

	p = fmt_bytes(p, e, psk, 32);
	*p++ = ' ';
	p = fmt_ssid(p, e, ssid, slen);

	if(load_config()) return;

	find_ssid(&ln, ssid, slen);
	save_line(&ln, buf, p - buf);
}

int drop_psk(byte* ssid, int slen)
{
	struct line ln;
	int ret;

	if((ret = load_config()) < 0)
		return ret;
	if((ret = find_ssid(&ln, ssid, slen)) < 0)
		return ret;

	drop_line(&ln);

	return 0;
}

/* Saving and loading current AP */

void load_state(void)
{
	int fd, rd, ret;
	char* name = WICAP;
	char buf[64];

	if((fd = open(name, O_RDONLY)) < 0)
		return;
	if((rd = read(fd, buf, sizeof(buf))) < 0)
		goto out;

	byte* ssid = (byte*)buf;
	int slen = rd;

	if((ret = set_fixed_saved(ssid, slen)) < 0)
		goto out;

	opermode = OP_ACTIVE;
	ap.fixed = 1;
out:
	close(fd);
	unlink(name);
}

void save_state(void)
{
	int fd;
	char* name = WICAP;
	char* buf = (char*)ap.ssid;
	int len = ap.slen;

	if(!ap.fixed)
		return;
	if((fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0)
		return;

	write(fd, buf, len);
	close(fd);
}
