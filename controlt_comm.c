#include "config.h"

#include <string.h>

#include "controlt_comm.h"

void init_header(struct ctrl_header *h, int cmd, int extra_len)
{
	memset(h, 0, sizeof(struct ctrl_header));

	h->magic = CNETD_MAGIC;
	h->version = CNETD_VERSION;
	h->len = sizeof(struct ctrl_header) + extra_len;
	h->command = cmd;
}