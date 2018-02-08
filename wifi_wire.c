#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/un.h>
#include <sys/file.h>
#include <sys/socket.h>

#include "common.h"
#include "control.h"
#include "nlusctl.h"
#include "wifi.h"

#define PAGE 4096

/* Socket init is split in two parts: socket() call is performed early so
   that it could be used to resolve netdev names into ifis, but connection
   is delayed until send_command() to avoid waking up wimon and then dropping
   the connection because of a local error. */

void init_heap_bufs(CTX)
{
	char* ucbuf = malloc(2048);

	ctx->uc.brk = ucbuf;
	ctx->uc.ptr = ucbuf;
	ctx->uc.end = ucbuf + 2048;

	char* rxbuf = malloc(2048);

	ctx->ur.buf = rxbuf;
	ctx->ur.mptr = rxbuf;
	ctx->ur.rptr = rxbuf;
	ctx->ur.end = rxbuf + 2048;
}

static int connctl(CTX, struct sockaddr_un* addr, int miss)
{
	int fd;

	if(ctx->fd > 0)
		close(ctx->fd);

	if((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		fail("socket AF_UNIX: %m\n");

	ctx->fd = fd;

	if(connect(ctx->fd, (void*)addr, sizeof(*addr)) < 0) {
		if(errno != -ENOENT || !miss)
			fail("connect %s: %m\n", addr->sun_path);
		else
			return -1;
	}

	ctx->connected = 1;

	return 0;
}

void connect_wictl(CTX)
{
	struct sockaddr_un wictl = { .sun_family = AF_UNIX, .sun_path = WICTL };

	connctl(ctx, &wictl, 0);
}

int connect_wictl_(CTX)
{
	struct sockaddr_un wictl = { .sun_family = AF_UNIX, .sun_path = WICTL };

	return connctl(ctx, &wictl, 1);
}

void send_command(CTX)
{
	int fd = ctx->fd;
	char* txbuf = ctx->uc.brk;
	int txlen = ctx->uc.ptr - ctx->uc.brk;

	if(!ctx->connected)
		fail("socket is not connected\n");

	if(writeall(fd, txbuf, txlen) < 0)
		fail("write: %m\n");
}

struct ucmsg* recv_reply(CTX)
{
	struct urbuf* ur = &ctx->ur;
	int ret, fd = ctx->fd;

	if((ret = uc_recv(fd, ur, 1)) < 0)
		return NULL;

	return ur->msg;
}

struct ucmsg* send_recv_msg(CTX)
{
	struct ucmsg* msg;

	send_command(ctx);

	while((msg = recv_reply(ctx)))
		if(msg->cmd <= 0)
			return msg;

	fail("connection lost\n");
}

int send_recv_cmd(CTX)
{
	struct ucmsg* msg = send_recv_msg(ctx);

	if(msg->cmd < 0)
		errno = -msg->cmd;

	return msg->cmd;
}

void send_check(CTX)
{
	if(send_recv_cmd(ctx) < 0)
		fail("send: %m\n");
}
