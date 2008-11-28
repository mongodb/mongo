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
#	handle<tab>
#	field<tab> 
#	conditions<tab>
#		open   -- can't be called after the handle's open method
#		verify -- call a subroutine to OK the argument
#	type
#
# In the type field @H is replaced by the handle, and @S is replaced by
# the "store" type.

db	-	errcall, void (*@S)(const @H *, const char *)
db	-	errfile, FILE *@S
db	-	errpfx, const char *@S
db	verify	maxitemsize, u_int32_t @S
db	verify	pagesize, u_int32_t @S	fragsize, u_int32_t @S, extentsize, u_int32_t @S
env	-	errcall, void (*@S)(const @H *, const char *)
env	-	errfile, FILE *@S
env	-	errpfx, const char *@S
