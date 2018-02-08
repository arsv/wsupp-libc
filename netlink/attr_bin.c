#include <stdlib.h>

#include "base.h"
#include "attr.h"

void* nl_bin(struct nlattr* at, unsigned len)
{
	return (at && at->len - sizeof(*at) == len) ? at->payload : NULL;
}
