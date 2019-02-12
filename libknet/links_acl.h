/*
 * Copyright (C) 2016-2018 Red Hat, Inc.  All rights reserved.
 *
 * Author: Christine Caulfield <ccaulfie@redhat.com>
 *
 * This software licensed under GPL-2.0+, LGPL-2.0+
 */

#ifndef __KNET_LINKS_ACL_H__
#define __KNET_LINKS_ACL_H__

#include "internals.h"

int ipcheck_validate(struct acl_match_entry **match_entry_head, struct sockaddr_storage *checkip);

void ipcheck_clear(struct acl_match_entry **match_entry_head);

int ipcheck_addip(struct acl_match_entry **match_entry_head,
		  struct sockaddr_storage *ip1, struct sockaddr_storage *ip2,
		  check_type_t type, check_acceptreject_t acceptreject);

#endif
