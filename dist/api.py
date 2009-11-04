# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$
#
# Read the api file and output C for the WT_TOC/DB structures, getter/setter
# functions, and other API initialization.

import os, string, sys
from dist import api_load, compare_srcfile

# Temporary file.
tmp_file = '__tmp'

# func_method_std --
#	Set methods to reference their underlying function.
def func_method_std(handle, method, config, f):
	f.write('\t' + handle + '->' +
	    method + ' = __wt_api_' + handle + '_' + method + ';\n')

# func_method_single --
#	Set methods to a single underlying function.
def func_method_single(handle, method, config, args, func, f):
	f.write('\t' + handle + '->' + method + ' = (')
	if config.count('methodV'):
		f.write('void')
	else:
		f.write('int')
	f.write(' (*)\n\t    ('  + handle.upper() + ' *')
	for l in args:
		f.write(', ' + l.split('/')[1].replace('@S', ''))
	f.write('))\n\t    __wt_' + handle + '_' + func + ';\n')

# func_method_lockout --
#	Set a handle's methods to the lockout function (skipping the close
#	method, it's always legal).
def func_method_lockout(handle, name, decl, f):
	f.write('void\n__wt_methods_' + name + '_lockout(' + decl + ')\n{\n')
	for i in sorted(filter(
	    lambda _i: _i[0].split('.')[1] != 'close' and
	    _i[0].count(handle), arguments.iteritems())):
		func_method_single(i[0].split('.')[0],
		    i[0].split('.')[1], config[i[0]], i[1], 'lockout', f)
	f.write('}\n\n')

# func_method_transition --
#	Write functions that transition a handle's methods on or off.
def func_method_transition(handle, name, decl, trans, f):
	# Build a list of the transitions.
	list={}
	if trans == 'off':
		t = off
	else:
		t = on
	for j in t.iteritems():
		for i in j[1]:
			list[i] = 1;

	# For each transition, build a function the performs it.
	for j in sorted(list):
		write_func = 0;
		s = 'void\n__wt_methods_' +\
		    name + '_' + j + '_' + trans + '(' + decl + ')\n{\n'
		for i in sorted(filter(lambda _i:
		    _i[0].count(handle) and t[_i[0]].count(j),
		    arguments.iteritems())):
			if write_func == 0:
				f.write(s);
				write_func = 1;
			if trans == 'off':
				func_method_single(
				    i[0].split('.')[0], i[0].split('.')[1],
				    config[i[0]], i[1], 'lockout', f)
			else:
				func_method_std(i[0].split('.')[0],
				    i[0].split('.')[1], config[i[0]], f)
		if write_func == 1:
			f.write('}\n\n')

# func_struct --
#	Output method name for a structure entry.
def func_struct(handle, method, config, args, f):
	f.write('\n\t')
	if config.count('methodV'):
		f.write('void')
	else:
		f.write('int')
	f.write(' (*' + method + ')(\n\t    ' + handle.upper() + ' *')
	for l in args:
		f.write(', ' + l.split('/')[1].replace('@S', ''))
	f.write(');\n')

# func_struct_all
#	Write out the struct entries for a handle's methods.
def func_struct_all(handle, f):
	for i in sorted(filter(
	    lambda _i: _i[0].count(handle), arguments.iteritems())):
		func_struct(i[0].split('.')[0],
		    i[0].split('.')[1], config[i[0]], arguments[i[0]], f)

# func_struct_variable
#	Output include file getter/setter variables for a method.
def func_struct_variable(args, f):
	f.write('\n')
	for l in args:
		f.write('\t' + l.split
		    ('/')[1].replace('@S', l.split('/')[0]) + ';\n')

# func_struct_variable_all
#	Output include file getter/setter variables for all methods.
def func_struct_variable_all(handle, f):
	for i in sorted(filter(lambda _i:
	    _i[0].count(handle) and config[_i[0]].count('setter'),
	    arguments.iteritems())):
		func_struct_variable(arguments[i[0]], f)

# func_method_decl --
#	Generate the API methods declaration.
def func_method_decl(handle, method, config, args, f):
	s = 'static '
	if config.count('methodV'):
		s += 'void'
	else:
		s += 'int'
	s += ' __wt_api_' + handle +\
	    '_' + method + '(\n\t' + handle.upper() + ' *' + handle
	for l in args:
		s += ',\n\t' +\
		    l.split('/')[1].replace('@S', l.split('/')[0])
	s += ')'
	f.write(s + ';\n')
	f.write(s + '\n{\n')

# func_method_getset --
#	Generate the getter/setter functions.
def func_method_getset(handle, method, config, args, f):
	func_method_decl(handle, method, config, args, f)

	if handle == 'db':
		f.write('\tENV *env = db->env;\n\n')
	f.write('\t__wt_lock(env, &env->ienv->mtx);')

	# Verify means call a standard verification routine because there are
	# constraints or side-effects on setting the value.  The setter fails
	# if the verification routine fails.
	if config.count('verify'):
		f.write('\n\tWT_RET((__wt_' +
		    handle + '_' + method + '_verify(' + handle)
		s = ''
		for l in args:
			s += ', ' + l.split('/')[0]
		s += ')'
		f.write(s + '));')
	f.write('\n')

	if config.count('getter'):
		for l in args:
			f.write('\t*(' + l.split('/')[0] + ')' + ' = ' +
			    handle + '->' + l.split('/')[0] + ';\n')
	else:
		for l in args:
			f.write('\t' + handle + '->' +
			    l.split('/')[0] + ' = ' + l.split('/')[0] + ';\n')
	f.write('\t__wt_unlock(&env->ienv->mtx);\n')
	f.write('\treturn (0);\n}\n\n')

#####################################################################
# Read in the api.py file.
#####################################################################
arguments, config, flags, off, on = api_load()

#####################################################################
# Update api.h, the API header file.
#####################################################################
tfile = open(tmp_file, 'w')
tfile.write('/* DO NOT EDIT: automatically built by dist/api.py. */\n\n')

tfile.write('/*\n')
tfile.write(' * Do not clear the DB handle in the WT_TOC schedule macro, we may be doing a\n')
tfile.write(' * WT_TOC call from within a DB call.\n')
tfile.write(' */\n')
tfile.write('#define\twt_api_toc_sched(oparg)\\\n')
tfile.write('\ttoc->op = (oparg);\\\n')
tfile.write('\ttoc->argp = &args;\\\n')
tfile.write('\treturn (__wt_toc_sched(wt_toc))\n')

tfile.write('#define\twt_api_db_sched(oparg)\\\n')
tfile.write('\ttoc->op = (oparg);\\\n')
tfile.write('\ttoc->db = db;\\\n')
tfile.write('\ttoc->argp = &args;\\\n')
tfile.write('\treturn (__wt_toc_sched(wt_toc))\n')

tfile.close()
compare_srcfile(tmp_file, '../inc_posix/api.h')

#####################################################################
# Update api.c, the API source file.
#####################################################################
tfile = open(tmp_file, 'w')
tfile.write('/* DO NOT EDIT: automatically built by dist/api.py. */\n\n')
tfile.write('#include "wt_internal.h"\n\n')

# Write the getter/setter methods.
for i in sorted(filter(lambda _i:
    config[_i[0]].count('getter') or config[_i[0]].count('setter'),
    arguments.iteritems())):
	func_method_getset(
	    i[0].split('.')[0],i[0].split('.')[1], config[i[0]], i[1], tfile)

# Write the method lockout and transition functions.
func_method_lockout('db.', 'db', 'DB *db', tfile)
func_method_transition('db.', 'db', 'DB *db', 'off', tfile)
func_method_transition('db.', 'db', 'DB *db', 'on', tfile)

func_method_lockout('env.', 'env', 'ENV *env', tfile)
func_method_transition('env.', 'env', 'ENV *env', 'off', tfile)
func_method_transition('env.', 'env', 'ENV *env', 'on', tfile)

func_method_lockout('wt_toc.', 'wt_toc', 'WT_TOC *wt_toc', tfile)
func_method_transition('wt_toc.', 'wt_toc', 'WT_TOC *wt_toc', 'off', tfile)
func_method_transition('wt_toc.', 'wt_toc', 'WT_TOC *wt_toc', 'on', tfile)

tfile.close()
compare_srcfile(tmp_file, '../support/api.c')

#####################################################################
# Update wiredtiger.in file with WT_TOC/DB methods and DB/ENV getter/setter
# variables.
#####################################################################
tfile = open(tmp_file, 'w')
skip = 0
for line in open('../inc_posix/wiredtiger.in', 'r'):
	if skip:
		if line.count('DB methods: END') or\
		    line.count('ENV methods: END') or\
		    line.count('WT_TOC methods: END') or\
		    line.count('DB getter/setter variables: END') or\
		    line.count('ENV getter/setter variables: END'):
			tfile.write('\t/*\n' + line)
			skip = 0
	else:
		tfile.write(line)
	if line.count('DB methods: BEGIN'):
		skip = 1
		tfile.write('\t */')
		func_struct_all('db.', tfile)
	elif line.count('ENV methods: BEGIN'):
		skip = 1
		tfile.write('\t */')
		func_struct_all('env.', tfile)
	elif line.count('WT_TOC methods: BEGIN'):
		skip = 1
		tfile.write('\t */')
		func_struct_all('wt_toc.', tfile)
	elif line.count('DB getter/setter variables: BEGIN'):
		skip = 1
		tfile.write('\t */')
		func_struct_variable_all('db.', tfile)
	elif line.count('ENV getter/setter variables: BEGIN'):
		skip = 1
		tfile.write('\t */')
		func_struct_variable_all('env.', tfile)

tfile.close()
compare_srcfile(tmp_file, '../inc_posix/wiredtiger.in')

os.remove(tmp_file)
