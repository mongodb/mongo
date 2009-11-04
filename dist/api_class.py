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
# The WiredTiger primary handles are the WT_TOC, the DB and the ENV.  The DB/ENV
# handles have the same functions as the Berkeley DB/ENV handles, that is, they
# reference a database and a database environment.  WiredTiger adds the WT_TOC
# handle, which identifies a single thread of control running in an environment.
#
# The initial line of each entry in the api file is the handle and method name,
# separated by a single '.'.
#
# Subsequent lines are a keyword, followed by additional information.  Possible
# keywords and their following information are as follows:
#
#	argument: method argument name/declaration
#		An argument to the function.  The "argument" keyword can be
#		listed multiple times, in the argument order, all of the
#		others are expected to be single lines.
#
#		In a declaration, @S is replaced by the argument name when
#		needed, sometimes we want the name, and sometimes we don't.
#
#	config: [arg, ] arg
#		getter	-- getter method
#		method	-- method returns an int
#		methodV -- method returns void
#		setter	-- setter method
#		verify  -- setters call a function to validate the arguments
#
#	flag: [flag-name,] flag-name
#		The name of a flag taken by this method or function.
#
#	off/on: [name, ] name
#		List of method transitions to/from allowed/not-allowed.

###################################################
# WT_TOC method declarations
###################################################
wt_toc.close
	config: method
	argument: flags/u_int32_t @S
	on: init

###################################################
# ENV method declarations
###################################################
env.cachesize_get
	config: method, getter
	argument: cachesize/u_int32_t *@S
	on: init
env.cachesize_set
	config: method, setter
	argument: cachesize/u_int32_t @S
	on: init

env.close
	config: method
	argument: flags/u_int32_t @S
	on: init

env.db
	config: method
	argument: flags/u_int32_t @S
	argument: dbp/DB **@S
	on: open

env.err
	config: methodV
	argument: err/int @S
	argument: fmt/const char *@S, ...
	on: init

env.errcall_get
	config: method, getter
	argument: errcall/void (**@S)(const ENV *, const char *)
	on: init
env.errcall_set
	config: method, setter
	argument: errcall/void (*@S)(const ENV *, const char *)
	on: init

env.errfile_get
	config: method, getter
	argument: errfile/FILE **@S
	on: init
env.errfile_set
	config: method, setter
	argument: errfile/FILE *@S
	on: init

env.errpfx_get
	config: method, getter
	argument: errpfx/const char **@S
	on: init
env.errpfx_set
	config: method, setter
	argument: errpfx/const char *@S
	on: init

env.errx
	config: methodV
	argument: fmt/const char *@S, ...
	on: init

env.open
	config: method
	argument: home/const char *@S
	argument: mode/mode_t @S
	argument: flags/u_int32_t @S
	on: init
	off: open

env.stat_clear
	config: method
	argument: flags/u_int32_t @S
	on: open

env.stat_print
	config: method
	argument: stream/FILE *@S
	argument: flags/u_int32_t @S
	on: open

env.toc
	config: method
	argument: flags/u_int32_t @S
	argument: tocp/WT_TOC **@S
	on: open

env.verbose_get
	config: method, getter
	argument: verbose/u_int32_t *@S
	on: init
env.verbose_set
	config: method, setter, verify
	argument: verbose/u_int32_t @S
	on: init

###################################################
# Db standalone method declarations
###################################################
db.btree_compare_get
	config: method, getter
	argument: btree_compare/int (**@S)(DB *, const DBT *, const DBT *)
	on: init
db.btree_compare_set
	config: method, setter
	argument: btree_compare/int (*@S)(DB *, const DBT *, const DBT *)
	off: open
	on: init

db.btree_compare_dup_get
	config: method, getter
	argument: btree_compare_dup/int (**@S)(DB *, const DBT *, const DBT *)
	on: init
db.btree_compare_dup_set
	config: method, setter
	argument: btree_compare_dup/int (*@S)(DB *, const DBT *, const DBT *)
	on: init
	off: open

db.btree_compare_int_get
	config: method, getter
	argument: btree_compare_int/int *@S
	on: init
db.btree_compare_int_set
	config: method, setter, verify
	argument: btree_compare_int/int @S
	on: init
	off: open

db.btree_dup_offpage_get
	config: method, getter
	argument: btree_dup_offpage/u_int32_t *@S
	on: init
db.btree_dup_offpage_set
	config: method, setter
	argument: btree_dup_offpage/u_int32_t @S
	on: init
	off: open

db.btree_itemsize_get
	config: method, getter
	argument: intlitemsize/u_int32_t *@S
	argument: leafitemsize/u_int32_t *@S
	on: init
db.btree_itemsize_set
	config: method, setter
	argument: intlitemsize/u_int32_t @S
	argument: leafitemsize/u_int32_t @S
	on: init
	off: open

db.btree_pagesize_get
	config: method, getter
	argument: allocsize/u_int32_t *@S
	argument: intlsize/u_int32_t *@S
	argument: leafsize/u_int32_t *@S
	argument: extsize/u_int32_t *@S
	on: init
db.btree_pagesize_set
	config: method, setter
	argument: allocsize/u_int32_t @S
	argument: intlsize/u_int32_t @S
	argument: leafsize/u_int32_t @S
	argument: extsize/u_int32_t @S
	on: init
	off: open

db.bulk_load
	config: method
	argument: flags/u_int32_t @S
	argument: cb/int (*@S)(DB *, DBT **, DBT **)
	on: open

db.close
	config: method
	argument: flags/u_int32_t @S
	on: init

db.dump
	config: method
	argument: stream/FILE *@S
	argument: flags/u_int32_t @S
	on: open

db.errcall_get
	config: method, getter
	argument: errcall/void (**@S)(const DB *, const char *)
	on: init
db.errcall_set
	config: method, setter
	argument: errcall/void (*@S)(const DB *, const char *)
	on: init

db.errfile_get
	config: method, getter
	argument: errfile/FILE **@S
	on: init
db.errfile_set
	config: method, setter
	argument: errfile/FILE *@S
	on: init

db.errpfx_get
	config: method, getter
	argument: errpfx/const char **@S
	on: init
db.errpfx_set
	config: method, setter
	argument: errpfx/const char *@S
	on: init

db.get
	config: method
	argument: toc/WT_TOC *@S
	argument: key/DBT *@S
	argument: pkey/DBT *@S
	argument: data/DBT *@S
	argument: flags/u_int32_t @S
	on: open

db.get_recno
	config: method
	argument: toc/WT_TOC *@S
	argument: recno/u_int64_t @S
	argument: key/DBT *@S
	argument: pkey/DBT *@S
	argument: data/DBT *@S
	argument: flags/u_int32_t @S
	on: open

db.open
	config: method
	argument: dbname/const char *@S
	argument: mode/mode_t @S
	argument: flags/u_int32_t @S
	off: open
	on: init

db.stat_clear
	config: method
	argument: flags/u_int32_t @S
	on: open

db.stat_print
	config: method
	argument: stream/FILE * @S
	argument: flags/u_int32_t @S
	on: open

db.sync
	config: method
	argument: flags/u_int32_t @S
	on: open

db.verify
	config: method
	argument: flags/u_int32_t @S
	on: open
