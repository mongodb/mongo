# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$
#
# A fair number of the getters/setters only get/set a single field of
# a handle.  Auto-generate the code that supports them.
#
# The fields of each line of the getset file:
#
#	handle<tab>conditions<tab>
#		verify -- call a subroutine to OK the argument
#	<tab>field<tab>type
#	...
#
# In the @S is replaced by the "store" name.

env	-
	errcall	void (*@S)(const ENV *, const char *)
env	-
	errfile	FILE *@S
env	-
	errpfx	const char *@S
env	verify
	verbose	u_int32_t @S

db	-
	errcall	void (*@S)(const DB *, const char *)
db	-
	errfile	FILE *@S
db	-
	errpfx	const char *@S
db	verify
	maxitemsize	u_int32_t @S
db	verify
	pagesize	u_int32_t @S
	extentsize	u_int32_t @S
