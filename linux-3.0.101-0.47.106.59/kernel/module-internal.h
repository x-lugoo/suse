/* Module internals
 *
 * Copyright (C) 2012 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

extern struct key *modsign_keyring;
#ifdef CONFIG_MODULE_SIG_BLACKLIST
extern struct key *modsign_blacklist;
#endif

extern int mod_verify_sig(const void *mod, unsigned long *_modlen, bool truncate_only);
