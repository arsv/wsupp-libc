#include <sys/file.h>
#include <unistd.h>
#include <errno.h>

#include "../common.h"

/* We return EPIPE here to indicate incomplete write.
   In all concievable case that should be the only possible
   cause (and we'll probably get SIGPIPE anyway) */

long writeall(int fd, void* buf, long len)
{
	long wr = 0;

	while(len > 0) {
		wr = write(fd, buf, len);

		if(!wr)
			errno = -EPIPE;
		if(wr <= 0)
			break;

		buf += wr;
		len -= wr;
	}

	return wr > 0 ? wr : -1;
}
