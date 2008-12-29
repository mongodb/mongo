# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$
#
# Auto-generate getter/setter code and handle method initialization.
#
# The fields of each line of the getset file:
#
#	handle<tab>conditions<tab>
#		open    -- unavailable until after the Db.open method call.
#		getset  -- a getter/setter declaration
#		method  -- a standalone method, returning int
#		methodV -- a standalone method, returning void
#		verify  -- getset: call a subroutine to OK the argument
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
env	getset
	cachesize	u_int32_t @S

###################################################
# Env standalone method declarations
###################################################
env	method
	close	u_int32_t
env	method
	destroy	u_int32_t
env	methodV
	err	int, const char *, ...
env	methodV
	errx	const char *, ...

###################################################
# Db getter/setter method declarations
###################################################
db	getset
	btree_compare	int (*@S)(DB *, const DBT *, const DBT *)
db	getset,verify
	btree_compare_int	int @S
db	getset
	dup_compare	int (*@S)(DB *, const DBT *, const DBT *)
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
db	method,open
	close	u_int32_t
db	method
	destroy	u_int32_t
db	method,open
	dump	FILE *, u_int32_t
db	methodV
	err	int, const char *, ...
db	methodV
	errx	const char *, ...
db	method,open
	get	DBT *, DBT *, DBT *, u_int32_t
db	method
	open	const char *, mode_t, u_int32_t
db	method
	stat_clear	u_int32_t
db	method
	stat_print	FILE *, u_int32_t
db	method,open
	sync	u_int32_t
db	method,open
	verify	u_int32_t
