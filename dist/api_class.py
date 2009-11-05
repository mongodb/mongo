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
# The api file is a python script that loads two dictionaries: "methods" and
# "flags".
#
# The "methods" dictionary is a set of API class objects, keyed by the
# method name.   The fields are:
#	1: the method name
#		 'handle' + '.' + 'method'
#	2: a string of comma-separated configuration key words
#		getter	-- getter method
#		method	-- method returns an int
#		methodV -- method returns void
#		setter	-- setter method
#		verify  -- setters call a function to validate the arguments
#	3: a list of argument and name/declaration pairs
#		An argument to the method.  In an argument declaration, "@S"
#		is replaced by the argument name when needed (sometimes we.
#		need the name in a declaration, and sometimes we don't).
#	4: a list of flags, if any.
#		If there's a flags variable, but the method doesn't currently
#		take any flags, enter '__NONE__'.
#	5: a list of on-transitions, if any
#	6: a list of off-transitions, if any

flags = {}
methods = {}

class Api:
	def __init__(self, method, config, args, f, on, off):
		self.handle = method.split('.')[0]
		self.method = method.split('.')[1]
		self.config = config
		self.args = args
		if f:
			flags[method] = f
		self.on = on
		self.off = off


###################################################
# WT_TOC method declarations
###################################################
methods['wt_toc.close'] = Api(
	'wt_toc.close',
	'method',
	['flags/u_int32_t @S'],
	['__NONE__'],
	['init'], [])

###################################################
# ENV method declarations
###################################################
methods['env.cachesize_get'] = Api(
	'env.cachesize_get',
	'method, getter',
	['cachesize/u_int32_t *@S'],
	[],
	['init'], [])
methods['env.cachesize_set'] = Api(
	'env.cachesize_set',
	'method, setter',
	['cachesize/u_int32_t @S'],
	[],
	['init'], [])

methods['env.close'] = Api(
	'env.close',
	'method',
	['flags/u_int32_t @S'],
	['__NONE__'],
	['init'], [])

methods['env.db'] = Api(
	'env.db',
	'method',
	['flags/u_int32_t @S', 'dbp/DB **@S'],
	['__NONE__'],
	['open'], [])

methods['env.err'] = Api(
	'env.err',
	'methodV',
	['err/int @S', 'fmt/const char *@S, ...'],
	[],
	['init'], [])

methods['env.errcall_get'] = Api(
	'env.errcall_get',
	'method, getter',
	['errcall/void (**@S)(const ENV *, const char *)'],
	[],
	['init'], [])
methods['env.errcall_set'] = Api(
	'env.errcall_set',
	'method, setter',
	['errcall/void (*@S)(const ENV *, const char *)'],
	[],
	['init'], [])

methods['env.errfile_get'] = Api(
	'env.errfile_get',
	'method, getter',
	['errfile/FILE **@S'],
	[],
	['init'], [])
methods['env.errfile_set'] = Api(
	'env.errfile_set',
	'method, setter',
	['errfile/FILE *@S'],
	[],
	['init'], [])

methods['env.errpfx_get'] = Api(
	'env.errpfx_get',
	'method, getter',
	['errpfx/const char **@S'],
	[],
	['init'], [])
methods['env.errpfx_set'] = Api(
	'env.errpfx_set',
	'method, setter',
	['errpfx/const char *@S'],
	[],
	['init'], [])

methods['env.errx'] = Api(
	'env.errx',
	'methodV',
	['fmt/const char *@S, ...'],
	[],
	['init'], [])

methods['env.open'] = Api(
	'env.open',
	'method',
	['home/const char *@S', 'mode/mode_t @S', 'flags/u_int32_t @S'],
	['__NONE__'],
	['init'], ['open'])

methods['env.stat_clear'] = Api(
	'env.stat_clear',
	'method',
	['flags/u_int32_t @S'],
	['__NONE__'],
	['init'], [])

methods['env.stat_print'] = Api(
	'env.stat_print',
	'method',
	['stream/FILE *@S', 'flags/u_int32_t @S'],
	['__NONE__'],
	['init'], [])

methods['env.toc'] = Api(
	'env.toc',
	'method',
	['flags/u_int32_t @S', 'tocp/WT_TOC **@S'],
	['__NONE__'],
	['open'], [])

methods['env.verbose_get'] = Api(
	'env.verbose_get',
	'method, getter',
	['verbose/u_int32_t *@S'],
	[],
	['init'], [])
methods['env.verbose_set'] = Api(
	'env.verbose_set',
	'method, setter, verify',
	['verbose/u_int32_t @S'],
	[
	'VERB_FILEOPS',
	'VERB_FILEOPS_ALL'],
	['init'], [])

###################################################
# Db standalone method declarations
###################################################
methods['db.btree_compare_get'] = Api(
	'db.btree_compare_get',
	'method, getter',
	['btree_compare/int (**@S)(DB *, const DBT *, const DBT *)'],
	[],
	['init'], [])
methods['db.btree_compare_set'] = Api(
	'db.btree_compare_set',
	'method, setter',
	['btree_compare/int (*@S)(DB *, const DBT *, const DBT *)'],
	[],
	['init'], ['open'])

methods['db.btree_compare_dup_get'] = Api(
	'db.btree_compare_dup_get',
	'method, getter',
	['btree_compare_dup/int (**@S)(DB *, const DBT *, const DBT *)'],
	[],
	['init'], [])
methods['db.btree_compare_dup_set'] = Api(
	'db.btree_compare_dup_set',
	'method, setter',
	['btree_compare_dup/int (*@S)(DB *, const DBT *, const DBT *)'],
	[],
	['init'], ['open'])

methods['db.btree_compare_int_get'] = Api(
	'db.btree_compare_int_get',
	'method, getter',
	['btree_compare_int/int *@S'],
	[],
	['init'], [])
methods['db.btree_compare_int_set'] = Api(
	'db.btree_compare_int_set',
	'method, setter, verify',
	['btree_compare_int/int @S'],
	[],
	['init'], ['open'])

methods['db.btree_dup_offpage_get'] = Api(
	'db.btree_dup_offpage_get',
	'method, getter',
	['btree_dup_offpage/u_int32_t *@S'],
	[],
	['init'], [])
methods['db.btree_dup_offpage_set'] = Api(
	'db.btree_dup_offpage_set',
	'method, setter',
	['btree_dup_offpage/u_int32_t @S'],
	[],
	['init'], ['open'])

methods['db.btree_itemsize_get'] = Api(
	'db.btree_itemsize_get',
	'method, getter',
	['intlitemsize/u_int32_t *@S', 'leafitemsize/u_int32_t *@S'],
	[],
	['init'], [])
methods['db.btree_itemsize_set'] = Api(
	'db.btree_itemsize_set',
	'method, setter',
	['intlitemsize/u_int32_t @S', 'leafitemsize/u_int32_t @S'],
	[],
	['init'], ['open'])

methods['db.btree_pagesize_get'] = Api(
	'db.btree_pagesize_get',
	'method, getter',
	['allocsize/u_int32_t *@S', 'intlsize/u_int32_t *@S',
	    'leafsize/u_int32_t *@S', 'extsize/u_int32_t *@S'],
	[],
	['init'], [])
methods['db.btree_pagesize_set'] = Api(
	'db.btree_pagesize_set',
	'method, setter',
	['allocsize/u_int32_t @S', 'intlsize/u_int32_t @S',
	    'leafsize/u_int32_t @S', 'extsize/u_int32_t @S'],
	[],
	['init'], ['open'])

methods['db.bulk_load'] = Api(
	'db.bulk_load',
	'method',
	['flags/u_int32_t @S', 'cb/int (*@S)(DB *, DBT **, DBT **)'],
	[
	'DUPLICATES',
	'SORTED_INPUT' ],
	['open'], [])

methods['db.close'] = Api(
	'db.close',
	'method',
	['flags/u_int32_t @S'],
	['__NONE__'],
	['init'], [])

methods['db.dump'] = Api(
	'db.dump',
	'method',
	['stream/FILE *@S', 'flags/u_int32_t @S'],
	[ 'DEBUG', 'PRINTABLES' ],
	['open'], [])

methods['db.errcall_get'] = Api(
	'db.errcall_get',
	'method, getter',
	['errcall/void (**@S)(const DB *, const char *)'],
	[],
	['init'], [])
methods['db.errcall_set'] = Api(
	'db.errcall_set',
	'method, setter',
	['errcall/void (*@S)(const DB *, const char *)'],
	[],
	['init'], [])

methods['db.errfile_get'] = Api(
	'db.errfile_get',
	'method, getter',
	['errfile/FILE **@S'],
	[],
	['init'], [])
methods['db.errfile_set'] = Api(
	'db.errfile_set',
	'method, setter',
	['errfile/FILE *@S'],
	[],
	['init'], [])

methods['db.errpfx_get'] = Api(
	'db.errpfx_get',
	'method, getter',
	['errpfx/const char **@S'],
	[],
	['init'], [])
methods['db.errpfx_set'] = Api(
	'db.errpfx_set',
	'method, setter',
	['errpfx/const char *@S'],
	[],
	['init'], [])

methods['db.get'] = Api(
	'db.get',
	'method',
	['toc/WT_TOC *@S', 'key/DBT *@S',
	    'pkey/DBT *@S', 'data/DBT *@S', 'flags/u_int32_t @S'],
	['__NONE__'],
	['open'], [])

methods['db.get_recno'] = Api(
	'db.get_recno',
	'method',
	['toc/WT_TOC *@S', 'recno/u_int64_t @S', 'key/DBT *@S',
	    'pkey/DBT *@S', 'data/DBT *@S', 'flags/u_int32_t @S'],
	['__NONE__'],
	['open'], [])

methods['db.open'] = Api(
	'db.open',
	'method',
	['dbname/const char *@S', 'mode/mode_t @S', 'flags/u_int32_t @S'],
	[ 'CREATE' ],
	['init'], [])

methods['db.stat_clear'] = Api(
	'db.stat_clear',
	'method',
	['flags/u_int32_t @S'],
	['__NONE__'],
	['open'], [])

methods['db.stat_print'] = Api(
	'db.stat_print',
	'method',
	['stream/FILE * @S', 'flags/u_int32_t @S'],
	['__NONE__'],
	['open'], [])

methods['db.sync'] = Api(
	'db.sync',
	'method',
	['flags/u_int32_t @S'],
	['__NONE__'],
	['open'], [])

methods['db.verify'] = Api(
	'db.verify',
	'method',
	['flags/u_int32_t @S'],
	['__NONE__'],
	['open'], [])

###################################################
# Structure and internal routine flag declarations
###################################################
flags['cache'] = [ 'INITIALIZED' ]
flags['dbt'] = [ 'ALLOCATED' ]
flags['ienv'] = [ 'RUNNING', 'SINGLE_THREADED' ]
flags['toc'] = [ 'INVALID', 'SINGLE_THREADED' ]
flags['wiredtiger_env_init'] = [ 'SINGLE_THREADED' ]
flags['wt_cache_in'] = [ 'UNFORMATTED' ]
flags['wt_cache_out'] = [ 'MODIFIED', 'UNFORMATTED' ]
flags['wt_indx'] = [ 'ALLOCATED' ]
flags['wt_page'] = [ 'ALLOCATED', 'MODIFIED' ]
