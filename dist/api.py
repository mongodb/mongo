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
from dist import compare_srcfile

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
def func_method_lockout(handle, decl, f):
	quiet = 1
	f.write('void\n__wt_methods_' + handle + '_lockout(' + decl + ')\n{\n')
	for i in sorted(filter(lambda _i:
	    _i[1].handle == handle and _i[1].method != 'close',
	    api.iteritems())):
		func_method_single(i[1].handle,
		    i[1].method, i[1].config, i[1].args, 'lockout', f)
		quiet = 0;
	if quiet:
		f.write('\tWT_CC_QUIET(' + handle + ', NULL);\n')
	f.write('}\n\n')

# func_method_transition --
#	Write functions that transition a handle's methods on or off.
def func_method_transition(handle, decl, f):
	# Build dictionaries of methods that need on/off transitions, keyed
	# by the name of the transition, and a list of the transition names.
	on={}
	off={}
	trans=[]
	for i in sorted(filter(lambda _i:
	    _i[1].handle == handle, api.iteritems())):
		for j in i[1].off:
			if j not in off:
				off[j] = []
			if j not in on:
				on[j] = []
			off[j].append(i[1])
		for j in i[1].on:
			if j not in trans:
				trans.append(j)
			if j not in off:
				off[j] = []
			if j not in on:
				on[j] = []
			on[j].append(i[1])

	# Write a transition function that turns methods on/off.
	for i in trans:
		f.write('void\n__wt_methods_' +\
		    handle + '_' + i + '_transition' + '(' + decl + ')\n{\n')
		for j in off[i]:
			func_method_single(
			    j.handle, j.method, j.config, j.args, 'lockout', f)
		for j in on[i]:
			func_method_std(j.handle, j.method, j.config, f)
		f.write('}\n\n')

# func_struct_all
#	Write out the struct entries for a handle's methods.
def func_struct_all(handle, f):
	for i in sorted(filter(lambda _i:
	    _i[1].handle.count(handle), api.iteritems())):
		func_struct(i[1], f)

# func_struct --
#	Output method name for a structure entry.
def func_struct(a, f):
	handle = a.handle
	method = a.method
	config = a.config
	args = a.args

	f.write('\n\t')
	if config.count('methodV'):
		f.write('void')
	else:
		f.write('int')
	f.write(' (*' + method + ')(\n\t    ' + handle.upper() + ' *')
	for l in args:
		f.write(', ' + l.split('/')[1].replace('@S', ''))
	f.write(');\n')

# func_struct_variable_all
#	Output include file getter/setter variables for all methods.
def func_struct_variable_all(handle, f):
	for i in sorted(filter(lambda _i:
	    _i[1].handle.count(handle) and
	    _i[1].config.count('setter') and not
	    _i[1].config.count('handcode'), api.iteritems())):
		func_struct_variable(i[1].args, f)

# func_struct_variable
#	Output include file getter/setter variables for a method.
def func_struct_variable(args, f):
	# Put blanks between each function's variables.
	f.write('\n')

	# Flags to the setter don't get propagated into the structure.
	for l in args:
		if not l.count('flags'):
			f.write('\t' + l.split
			    ('/')[1].replace('@S', l.split('/')[0]) + ';\n')

# func_method_decl --
#	Generate the API methods declaration.
def func_method_decl(a, f):
	handle = a.handle
	method = a.method
	config = a.config
	args = a.args

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
def func_method_getset(a, f):
	func_method_decl(a, f)

	handle = a.handle
	method = a.method
	config = a.config
	args = a.args

	# Declarations:
	# If we don't have an environment handle, acquire one.
	# If we are hand-coding the routine, we'll need a place to save the
	# return value.
	newl = 0
	if handle != 'env':
		f.write('\tENV *env;\n')
		newl = 1
	if config.count('handcode'):
		f.write('\tint ret;\n')
		newl = 1
	if newl:
		f.write('\n')
	if handle != 'env':
		f.write('\tenv = ' + handle + '->env;\n\n')

	# If we have a "flags" argument to a setter fucntion, check it
	# before we continue.
	if config.count('setter'):
		for l in args:
			if l.count('flags/'):
				f.write('\tWT_ENV_FCHK(env, "' +
				    handle.upper() + '.' + method +
				    '",\n\t    ' + l.split('/')[0]  +
				    ', WT_APIMASK_' + handle.upper() +
				    '_' + method.upper() + ');\n\n')
				break

	# Verify means call a standard verification routine because there are
	# constraints or side-effects on setting the value.  The setter fails
	# if the verification routine fails.
	if config.count('verify'):
		f.write('\tWT_RET((__wt_' +
		    handle + '_' + method + '_verify(' + handle)
		s = ''
		for l in args:
			s += ', ' + l.split('/')[0]
		s += ')'
		f.write(s + '));\n\n')

	# getter/setter implies ienvlock: lock the data structure.
	f.write('\t__wt_lock(env, &env->ienv->mtx);\n')

	# If the function is hand-coded, just call it.
	if config.count('handcode'):
		f.write('\tret = __wt_' +
		    handle + '_' + method + '(\n\t    ' + handle)
		for l in args:
			f.write(', ' + l.split('/')[0])
		f.write(');\n')
	elif config.count('getter'):
		for l in args:
			if l.count('flags/') and flags[a.key][0] == '__NONE__':
				continue
			f.write('\t*' + l.split('/')[0] + ' = ' +
			    handle + '->' + l.split('/')[0] + ';\n')
	else:
		for l in args:
			if l.count('flags/') and flags[a.key][0] == '__NONE__':
				continue
			f.write('\t' + handle + '->' +
			    l.split('/')[0] + ' = ' + l.split('/')[0] + ';\n')

	# getter/setter implies ienvlock: unlock the data structure.
	f.write('\t__wt_unlock(&env->ienv->mtx);\n')
	f.write('\treturn (')
	if config.count('handcode'):
		f.write('ret')
	else:
		f.write('0')
	f.write(');\n}\n\n')

# func_method --
#	Generate all other API entry functions.
def func_method(a, f):
	func_method_decl(a, f)

	handle = a.handle
	method = a.method
	config = a.config
	args = a.args

	locking = config.count('ienvlock')	# We're doing locking
	restart = config.count('restart')	# We're handling WT_RESTART

	# We need an ENV handle, find one.
	# If we're doing locking or handling restart we need a 'ret' variable.
	if handle != 'env':
		f.write('\tENV *env;\n')
	if locking or restart:
		f.write('\tint ret;\n')
	if handle != 'env':
		f.write('\n\tenv = ' + handle + '->env;\n')
	if handle != 'env' or locking or restart:
		f.write('\n')

	# If we have a "flags" argument, check it before we continue.
	for l in args:
		if l.count('flags/'):
			f.write('\tWT_ENV_FCHK(env, "' + handle.upper() +
			    '.' + method + '", flags, WT_APIMASK_' +
			    handle.upper() + '_' + method.upper() + ');\n\n')
			break

	if locking:
		f.write('\t__wt_lock(env, &env->ienv->mtx);\n')
		f.write('\tret = ')
	elif restart:
		f.write('\twhile ((ret = ')
	else:
		f.write('\treturn (')
	f.write('__wt_' + handle + '_' + method + '(' + handle)
	for l in args:
		if l.count('flags/') and flags[a.key][0] == '__NONE__':
			continue
		f.write(', ' + l.split('/')[0])
	if restart:
		f.write(')) == WT_RESTART)\n\t\t;\n')

	if locking:
		f.write(');\n')
		f.write('\t__wt_unlock(&env->ienv->mtx);\n')
	if locking or restart:
		f.write('\treturn (ret);\n')
	else:
		f.write('));\n')
	f.write('}\n\n')


#####################################################################
# Build the API dictionary.
#####################################################################
import api_class
api = api_class.methods
flags = api_class.flags

#####################################################################
# Update api.c, the API source file.
#####################################################################
tfile = open(tmp_file, 'w')
tfile.write('/* DO NOT EDIT: automatically built by dist/api.py. */\n\n')
tfile.write('#include "wt_internal.h"\n\n')

# We need a function for any getter/setter method, as well as any method that
# takes a "flags" or "toc" argument.
for i in sorted(api.iteritems()):
	if i[1].config.count('noauto'):
		continue
	if i[1].config.count('getter') or i[1].config.count('setter'):
		func_method_getset(i[1], tfile)
	else:
		func_method(i[1], tfile)

# Write the method lockout and transition functions.
func_method_lockout('db', 'DB *db', tfile)
func_method_transition('db', 'DB *db', tfile)

func_method_lockout('env', 'ENV *env', tfile)
func_method_transition('env', 'ENV *env', tfile)

func_method_lockout('wt_toc', 'WT_TOC *wt_toc', tfile)
func_method_transition('wt_toc', 'WT_TOC *wt_toc', tfile)

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
		func_struct_all('db', tfile)
	elif line.count('ENV methods: BEGIN'):
		skip = 1
		tfile.write('\t */')
		func_struct_all('env', tfile)
	elif line.count('WT_TOC methods: BEGIN'):
		skip = 1
		tfile.write('\t */')
		func_struct_all('wt_toc', tfile)
	elif line.count('DB getter/setter variables: BEGIN'):
		skip = 1
		tfile.write('\t */')
		func_struct_variable_all('db', tfile)
	elif line.count('ENV getter/setter variables: BEGIN'):
		skip = 1
		tfile.write('\t */')
		func_struct_variable_all('env', tfile)

tfile.close()
compare_srcfile(tmp_file, '../inc_posix/wiredtiger.in')

os.remove(tmp_file)
