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
#		 getset -- a getter/setter declaration
#		 method -- a standalone method, returning int
#		 method -- a standalone method, returning void
#		 verify -- getset: call a subroutine to OK the argument
#	<tab>field<tab>type
#	...
#
# In the @S is replaced by the "store" name.

###################################################
# Env getter/setter method declarations
###################################################
env	getset
	errcall	void (*@S)(const ENV *, const char *)
env	getset
	errfile	FILE *@S
env	getset
	errpfx	const char *@S
env	getset,verify
	verbose	u_int32_t @S

###################################################
# Env standalone method declarations
###################################################
env	method
	destroy	u_int32_t
env	voidmethod
	err	int, const char *, ...
env	voidmethod
	errx	const char *, ...

###################################################
# Db getter/setter method declarations
###################################################
db	getset
	btree_compare	int (*@S)(DB *, const DBT *, const DBT *)
db	getset,verify
	btree_compare_int	int @S
db	getset
	errcall	void (*@S)(const DB *, const char *)
db	getset
	errfile	FILE *@S
db	getset
	errpfx	const char *@S
db	getset,verify
	pagesize	u_int32_t @S
	fragsize	u_int32_t @S
	extentsize	u_int32_t @S
	maxitemsize	u_int32_t @S

###################################################
# Db standalone method declarations
###################################################
db	method
	bulk_load	u_int32_t, int (*)(DB *, DBT **, DBT **)
db	method
	destroy	u_int32_t
db	method
	dump	FILE *, u_int32_t
db	voidmethod
	err	int, const char *, ...
db	voidmethod
	errx	const char *, ...
db	method
	open	const char *, mode_t, u_int32_t
