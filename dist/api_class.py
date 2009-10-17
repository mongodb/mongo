# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$
#
# Auto-generate everything we can:
#	flag values
#	getter/setter code
#	manual page headers
#	structure method fields
#
# There are 2 primary handles: DB, WT_TOC.  The DB handle has the same function
# as the Berkeley DB handle of the same name: the DB is a single table/database.
# WiredTiger adds the WT_TOC handle, which identifies a single thread of control
# running in an environment.  (There is an ENV structure in WiredTiger, but it's
# not currently used by applications.)   In other words, instead of surfacing an
# ENV handle as in Berkeley DB, WiredTiger surfaces a thread-of-control handle
# which has a reference to its environment.
#
# The initial line of each entry in the api file is the handle and method name,
# separated by a single '.'.  The handles are either "toc" (a WT_TOC method),
# or "db" (a DB method).
#
# Subsequent lines are a keyword, followed by additional information.  Possible
# keywords and their following information are as follows:
#
#	argument: method argument name<tab>declaration
#
#		In a declaration, @S is replaced by the argument name -- we
#		do that because the name goes in different places in each
#		declaration, and sometimes we want the name, and sometimes
#		we don't.
#
#	config: [arg,] arg
#		getset  -- an API getter/setter method
#		local	-- no need to connect to the server[1]
#		method	-- a method returning an int
#		methodV -- a method returning void
#		notoc	-- method doesn't take a WT_TOC reference
#		open    -- illegal until after the handle.open method call
#		verify  -- setters call a subroutine to validate the arguments
#
#	flag: [flag-name,] flag-name
#
# ***
# Currently, only the "argument" keyword can be listed multiple times, all the
# others are expected to be single lines.
#
# [1]
# Don't mark API methods as "local" unless you're sure -- what this keyword
# means is that multiple threads of control can call the function without any
# locking on the underlying structures being manipulated.  (It's not that the
# server does "locking", but simply by switching to a server, you've serialized
# access to the underlying structure.)
env.open
	config: method
	argument: home	const char *@S
	argument: mode	mode_t @S
	argument: flags	u_int32_t @S

env.close
	config: method
	argument: flags	u_int32_t @S

env.db_create
	config: method
	argument: flags	u_int32_t @S
	argument: dbp	DB **@S

env.destroy
	config: method,  local, notoc
	argument: flags	u_int32_t @S

env.err
	config: methodV, local, notoc
	argument: err	int @S
	argument: fmt	const char *@S, ...

env.errx
	config: methodV, local, notoc
	argument: fmt	const char *@S, ...

env.start
	config: method, local, notoc
	argument: flags	u_int32_t @S

env.stat_clear
	config: method
	argument: flags	u_int32_t @S

env.stat_print
	config: method
	argument: stream	FILE *@S
	argument: flags	u_int32_t @S

env.stop
	config: method, local, notoc
	argument: flags	u_int32_t @S

env.toc_create
	config: method, local, notoc
	argument: flags	u_int32_t @S
	argument: tocp	WT_TOC **@S

###################################################
# Env getter/setter method declarations
###################################################
env.get_errcall
	config: method, getset
	argument: errcall	void (**@S)(const ENV *, const char *)
env.set_errcall
	config: method, getset
	argument: errcall	void (*@S)(const ENV *, const char *)

env.get_errfile
	config: method, getset
	argument: errfile	FILE **@S
env.set_errfile
	config: method, getset
	argument: errfile	FILE *@S

env.get_errpfx
	config: method, getset
	argument: errpfx	const char **@S
env.set_errpfx
	config: method, getset
	argument: errpfx	const char *@S

env.get_verbose
	config: method, getset
	argument: verbose	u_int32_t *@S
env.set_verbose
	config: method, getset, verify
	argument: verbose	u_int32_t @S

env.get_cachesize
	config: method, getset
	argument: cachesize	u_int32_t *@S
env.set_cachesize
	config: method, getset
	argument: cachesize	u_int32_t @S

###################################################
# WT_TOC method declarations
###################################################
wt_toc.destroy
	config: method, local, notoc
	argument: flags	u_int32_t @S

###################################################
# Db standalone method declarations
###################################################
db.bulk_load
	config: method
	argument: flags	u_int32_t @S
	argument: cb	int (*@S)(DB *, DBT **, DBT **)

db.close
	config: method
	argument: flags	u_int32_t @S

db.destroy
	config: method
	argument: flags	u_int32_t @S

db.dump
	config: method, open
	argument: stream	FILE *@S
	argument: flags	u_int32_t @S

db.err
	config: methodV, local, notoc
	argument: err	int @S
	argument: fmt	const char *@S, ...

db.errx
	config: methodV, local, notoc
	argument: fmt	const char *@S, ...

db.get
	config: method, local, open
	argument: key	DBT *@S
	argument: pkey	DBT *@S
	argument: data	DBT *@S
	argument: flags	u_int32_t @S

db.get_stoc
	config: method, open
	argument: key	DBT *@S
	argument: pkey	DBT *@S
	argument: data	DBT *@S
	argument: flags	u_int32_t @S

db.get_recno
	config: method, local, open
	argument: recno	u_int64_t @S
	argument: key	DBT *@S
	argument: pkey	DBT *@S
	argument: data	DBT *@S
	argument: flags	u_int32_t @S

db.get_recno_stoc
	config: method, open
	argument: recno	u_int64_t @S
	argument: key	DBT *@S
	argument: pkey	DBT *@S
	argument: data	DBT *@S
	argument: flags	u_int32_t @S

db.open
	config: method
	argument: dbname	const char *@S
	argument: mode	mode_t @S
	argument: flags	u_int32_t @S

db.stat_clear
	config: method
	argument: flags	u_int32_t @S

db.stat_print
	config: method
	argument: stream	FILE * @S
	argument: flags	u_int32_t @S

db.sync
	config: method, open
	argument: flags	u_int32_t @S

db.verify
	config: method, open
	argument: flags	u_int32_t @S

###################################################
# Db getter/setter method declarations
###################################################
db.get_errcall
	config: method, getset
	argument: errcall	void (**@S)(const DB *, const char *)
db.set_errcall
	config: method, getset
	argument: errcall	void (*@S)(const DB *, const char *)

db.get_errfile
	config: method, getset
	argument: errfile	FILE **@S
db.set_errfile
	config: method, getset
	argument: errfile	FILE *@S

db.get_errpfx
	config: method, getset
	argument: errpfx	const char **@S
db.set_errpfx
	config: method, getset
	argument: errpfx	const char *@S

db.get_btree_compare
	config: method, getset
	argument: btree_compare	int (**@S)(DB *, const DBT *, const DBT *)
db.set_btree_compare
	config: method, getset
	argument: btree_compare	int (*@S)(DB *, const DBT *, const DBT *)

db.get_btree_compare_int
	config: method, getset
	argument: btree_compare_int	int *@S
db.set_btree_compare_int
	config: method, getset, verify
	argument: btree_compare_int	int @S

db.get_btree_dup_compare
	config: method, getset
	argument: btree_dup_compare	int (**@S)(DB *, const DBT *, const DBT *)
db.set_btree_dup_compare
	config: method, getset
	argument: btree_dup_compare	int (*@S)(DB *, const DBT *, const DBT *)

db.get_btree_itemsize
	config: method, getset
	argument: intlitemsize	u_int32_t *@S
	argument: leafitemsize	u_int32_t *@S
db.set_btree_itemsize
	config: method, getset
	argument: intlitemsize	u_int32_t @S
	argument: leafitemsize	u_int32_t @S

db.get_btree_pagesize
	config: method, getset
	argument: allocsize	u_int32_t *@S
	argument: intlsize	u_int32_t *@S
	argument: leafsize	u_int32_t *@S
	argument: extsize	u_int32_t *@S
db.set_btree_pagesize
	config: method, getset
	argument: allocsize	u_int32_t @S
	argument: intlsize	u_int32_t @S
	argument: leafsize	u_int32_t @S
	argument: extsize	u_int32_t @S

db.get_btree_dup_offpage
	config: method, getset
	argument: btree_dup_offpage	u_int32_t *@S
db.set_btree_dup_offpage
	config: method, getset
	argument: btree_dup_offpage	u_int32_t @S
