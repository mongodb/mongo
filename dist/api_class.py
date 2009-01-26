# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$
#
# Auto-generate getter/setter code and handle method initialization.
#
# The initial line of each entry in the api file:
#
#	handle.method name
#		<tab>flag[,flag]
#		<tab>argument	<tab>type
#
#	handles:
#		env	-- DbEnv handle
#		db	-- Db handle
#	flags:
#		open    -- illegal until after the handle.open method call
#		getset  -- an API getter/setter method pair; getters return
#			   void, setters return int
#		method  -- a method returning int
#		methodV -- a method returning void
#		verify  -- setters call a subroutine to validate the arguments
#
# For standalone methods (the method and methodV flags), the next line is the
# prototype for the function.
#
# For getter/setter methods (the getset flag), the next lines are the argument
# names and declarations for the getter/setter.  In getter/setter declarations
# @S is replaced by the argument name.

###################################################
# Env getter/setter method declarations
###################################################
env.errcall
	getset
	errcall	void (*@S)(const ENV *, const char *)
env.errfile
	getset
	errfile	FILE *@S
env.errpfx
	getset
	errpfx	const char *@S
env.verbose
	getset,verify
	verbose	u_int32_t @S
env.cachesize
	getset
	cachesize	u_int32_t @S

###################################################
# Env standalone method declarations
###################################################
env.close
	method
	u_int32_t
env.destroy
	method
	u_int32_t
env.err
	methodV
	int, const char *, ...
env.errx
	methodV
	const char *, ...
env.open
	method
	const char *, mode_t, u_int32_t
env.stat_clear
	method
	u_int32_t
env.stat_print
	method
	FILE *, u_int32_t

###################################################
# Db getter/setter method declarations
###################################################
db.errcall
	getset
	errcall	void (*@S)(const DB *, const char *)
db.errfile
	getset
	errfile	FILE *@S
db.errpfx
	getset
	errpfx	const char *@S
db.btree_compare
	getset
	btree_compare	int (*@S)(DB *, const DBT *, const DBT *)
db.btree_compare_int
	getset,verify
	btree_compare_int	int @S
db.btree_dup_compare
	getset
	btree_dup_compare	int (*@S)(DB *, const DBT *, const DBT *)
db.btree_itemsize
	getset
	intlitemsize	u_int32_t @S
	leafitemsize	u_int32_t @S
db.btree_pagesize
	getset
	allocsize	u_int32_t @S
	intlsize	u_int32_t @S
	leafsize	u_int32_t @S
	extsize	u_int32_t @S
db.btree_dup_offpage
	getset
	btree_dup_offpage	u_int32_t @S

###################################################
# Db standalone method declarations
###################################################
db.bulk_load
	method
	u_int32_t, int (*)(DB *, DBT **, DBT **)
db.close
	method
	u_int32_t
db.destroy
	method
	u_int32_t
db.dump
	method,open
	FILE *, u_int32_t
db.err
	methodV
	int, const char *, ...
db.errx
	methodV
	const char *, ...
db.get
	method,open
	DBT *, DBT *, DBT *, u_int32_t
db.open
	method
	const char *, mode_t, u_int32_t
db.stat_clear
	method
	u_int32_t
db.stat_print
	method
	FILE *, u_int32_t
db.sync
	method,open
	u_int32_t
db.verify
	method,open
	u_int32_t
