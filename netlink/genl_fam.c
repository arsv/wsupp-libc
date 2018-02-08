#include <errno.h>

#include "ctx.h"
#include "attr.h"
#include "pack.h"
#include "genl.h"
#include "genl/ctrl.h"

/* No group subscription, just query the family name */

int query_family(struct netlink* nl, const char* name)
{
	struct nlgen* gen;
	uint16_t* u16;

	nl_new_cmd(nl, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 1);
	nl_put_str(nl, CTRL_ATTR_FAMILY_NAME, name);

	if(!(gen = nl_send_recv_genl(nl)))
		return nl->err;

	if(!(u16 = nl_get_u16(gen, CTRL_ATTR_FAMILY_ID)))
		return -EBADMSG;

	return *u16;
}
