# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$
#
# Read the api file and output C for the Db/Env structures, getter/setter
# functions, and other API initialization.

import os, string, sys
from collections import defaultdict
from dist import compare_srcfile

# api is a dictionary of lists, with the handle + method name as the key.  The
# list's first element is the flags, subsequent list elements are argument/type
# pairs for getter/setter methods, and method prototypes for all other methods.
api = defaultdict(list)

# func_input --
#	Read/parse the api file into the api dictionary.
def func_input():
	for line in open('api', 'r').readlines():
		# Skip comments and empty lines.
		if line[:1] == '#' or line[:1] == '\n':
			continue

		# Lines beginning with a tab are additional information for the
		# current method, all other lines are new methods.
		if line[:1] == '\t':
			api[method].append(line.strip())
		else:
			method = line.strip()

# func_stdcast --
#	Set methods to reference a single underlying function (usually an
#	error function).
def func_stdcast(handle, method, flags, rettype, func, args, f):
	f.write('\t' + handle + '->' + method +\
	    ' = (' + rettype + ' (*)\n\t    (' + handle.upper() + ' *')
	if flags.count('getset'):
		if method.count('get_'):
			for l in args:
				f.write(', ' +\
				    l.split('\t')[1].replace('@S', '*'))
		else:
			for l in args:
				f.write(', ' +\
				    l.split('\t')[1].replace('@S', ''))
	else:
		for l in args:
			f.write(', ' + l)
	f.write('))\n\t    __wt_' + handle + '_' + func + ';\n')

# func_method_init --
#	Set methods to reference their normal underlying function.
def func_method_init(handle, method, flags, args, f):
	# If it's a getter/setter, initialize both methods; ignore the open
	# keyword, getter/setter functions are always OK.
	if flags.count('getset'):
		s = '\t' + handle + '->get_' +\
		    method + ' = __wt_' + handle + '_get_' + method + ';\n'
		f.write(s + s.replace('get_', 'set_'))

	# If the open keyword is set, lock out the function for now.
	elif flags.count('open'):
		func_stdcast(\
		    handle, method, flags, 'int', 'lockout_open', args, f)

	# Otherwise, initialize the method handle for real.
	else:
		f.write('\t' + handle +\
		    '->' + method + ' = __wt_' + handle + '_' + method + ';\n')

# func_method_open --
#	Update methods to reference their normal underlying function (after
#	the open method is called).
def func_method_open(handle, method, flags, args, f):
	# If the open keyword is set, we need to reset the method.
	if flags.count('open'):
		f.write('\t' + handle +\
		    '->' + method + ' = __wt_' + handle + '_' + method + ';\n')

# func_method_lockout --
#	Set methods (other than destroy) to a single underlying error function.
def func_method_lockout(handle, method, flags, args, f):
	# Skip the destroy method, it's the only legal method.
	if method.count('destroy'):
		return;

	if flags.count('getset'):
		func_stdcast(handle,
		    'get_' + method, flags, 'void', 'lockout_err', args, f)
		func_stdcast(handle,
		    'set_' + method, flags, 'int', 'lockout_err', args, f)
	else:
		if flags.count('methodV'):
			rettype = 'void'
		else:
			rettype = 'int'
		func_stdcast(handle,
		    method, flags, rettype, 'lockout_err', args, f)

# func_decl --
#	Output method name and getter/setter variables for an include file.
def func_decl(handle, method, flags, args, f):

	if flags.count('getset'):
		for l in args:
			f.write('\t' + l.split\
			    ('\t')[1].replace('@S', l.split('\t')[0]) + ';\n')
		f.write('\tvoid (*get_' +\
		    method + ')(\n\t    ' + handle.upper() + ' *')
		for l in args:
			f.write(', ' + l.split('\t')[1].replace('@S', '*'))
		f.write(');\n')
		f.write('\tint (*set_' +\
		    method + ')(\n\t    ' + handle.upper() + ' *')
		for l in args:
			f.write(', ' + l.split('\t')[1].replace('@S', ''))
		f.write(');\n\n')
	else:
		if flags.count('methodV'):
			rettype = 'void'
		else:
			rettype = 'int'
		f.write('\t' + rettype + \
		    ' (*' + method + ')(\n\t    ' + handle.upper() + ' *, ')
		for l in args:
			f.write(l.replace('@S', '*'))
		f.write(');\n\n')

# func_getset --
#	Generate getter/setter code for the API.
def func_getset(handle, method, flags, args, f):
	# Write the getter function.
	f.write('static void __wt_' +\
	    handle + '_get_' + method + '(\n\t' + handle.upper() + ' *')
	for l in args:
		f.write(',\n\t' + \
		    l.split('\t')[1].replace('@S', '*'))
	f.write(');\n')
	f.write('static void\n__wt_' +\
	    handle + '_get_' + method + '(\n\t' + handle.upper() + ' *handle')
	for l in args:
		f.write(',\n\t' + l.split('\t')[1].\
		    replace('@S', '*' + l.split('\t')[0] + 'p'))
	f.write(')\n{\n')
	for l in args:
		f.write('\t*' + l.split('\t')[0] +\
		    'p = ' + 'handle->' + l.split('\t')[0] + ';\n')
	f.write('}\n\n')

	# Write the setter function.
	f.write('static int __wt_' +\
	    handle + '_set_' + method + '(\n\t' + handle.upper() + ' *')
	for l in args:
		f.write(',\n\t' + l.split('\t')[1].replace('@S', ''))
	f.write(');\n')
	f.write('static int\n__wt_' +\
	    handle + '_set_' + method + '(\n\t' + handle.upper() + ' *handle')
	for l in args:
		f.write(',\n\t' +\
		    l.split('\t')[1].replace('@S', l.split('\t')[0]))
	f.write(')\n{\n')

	# Verify means call a standard verification routine because there are
	# constraints or side-effects on setting the value.  The setter fails
	# if the verification routine fails.
	if flags.count('verify'):
		f.write('\tint ret;\n\n')
		f.write('\tif ((ret = __wt_' +\
		    handle + '_set_' + method + '_verify(\n\t    handle')
		for l in args:
			f.write(', &' + l.split('\t')[0])
		f.write(')) != 0)\n\t\treturn (ret);\n\n')

	for l in args:
		f.write('\thandle->' +\
		    l.split('\t')[0] + ' = ' + l.split('\t')[0] + ';\n')
	f.write('\treturn (0);\n}\n\n')

#####################################################################
# Read in the api.py file.
#####################################################################
func_input()

#####################################################################
# Update api.c.
#####################################################################
tmp_file = '__tmp'
tfile = open(tmp_file, 'w')
tfile.write('/* DO NOT EDIT: automatically built by dist/api.py. */\n\n')
tfile.write('#include "wt_internal.h"\n\n')

# Write the Env/Db getter/setter functions.
for i in filter(lambda _i:\
    _i[0].count('env') and _i[1][0].count('getset'), api.iteritems()):
	func_getset('env', i[0].split('.')[1], i[1][0], i[1][1:], tfile)

for i in filter(lambda _i:\
    _i[0].count('db') and _i[1][0].count('getset'), api.iteritems()):
	func_getset('db', i[0].split('.')[1], i[1][0], i[1][1:], tfile)

# Write the Env/Db method configuration functions.
tfile.write('void\n__wt_env_config_methods(ENV *env)\n{\n')
for i in filter(lambda _i: _i[0].count('env'), api.iteritems()):
	func_method_init('env', i[0].split('.')[1], i[1][0], i[1][1:], tfile)
tfile.write('}\n\n')
tfile.write('void\n__wt_env_config_methods_open(ENV *env)\n{\n')
for i in filter(lambda _i: _i[0].count('env'), api.iteritems()):
	func_method_open('env', i[0].split('.')[1], i[1][0], i[1][1:], tfile)
tfile.write('}\n\n')
tfile.write('void\n__wt_env_config_methods_lockout(ENV *env)\n{\n')
for i in filter(lambda _i: _i[0].count('env'), api.iteritems()):
	func_method_lockout('env', i[0].split('.')[1], i[1][0], i[1][1:], tfile)
tfile.write('}\n\n')

tfile.write('void\n__wt_db_config_methods(DB *db)\n{\n')
for i in filter(lambda _i: _i[0].count('db'), api.iteritems()):
	func_method_init('db', i[0].split('.')[1], i[1][0], i[1][1:], tfile)
tfile.write('}\n\n')
tfile.write('void\n__wt_db_config_methods_open(DB *db)\n{\n')
for i in filter(lambda _i: _i[0].count('db'), api.iteritems()):
	func_method_open('db', i[0].split('.')[1], i[1][0], i[1][1:], tfile)
tfile.write('}\n')
tfile.write('void\n__wt_db_config_methods_lockout(DB *db)\n{\n')
for i in filter(lambda _i: _i[0].count('db'), api.iteritems()):
	func_method_lockout('db', i[0].split('.')[1], i[1][0], i[1][1:], tfile)
tfile.write('}\n\n')

tfile.close()
compare_srcfile(tmp_file, '../support/api.c')

#####################################################################
# Update wiredtiger.in file with Env and Db handle information.
#####################################################################
tfile = open(tmp_file, 'w')
skip = 0
for line in open('../inc_posix/wiredtiger.in', 'r'):
	if skip:
		if line.count('Env handle api section: END') or \
		    line.count('Db handle api section: END'):
			tfile.write('\t/*\n' + line)
			skip = 0
	else:
		tfile.write(line)
	if line.count('Env handle api section: BEGIN'):
		skip = 1
		tfile.write('\t */\n')
		for i in filter(lambda _i: _i[0].count('env'), api.iteritems()):
			func_decl('env',
			    i[0].split('.')[1], i[1][0], i[1][1:], tfile)
	elif line.count('Db handle api section: BEGIN'):
		skip = 1
		tfile.write('\t */\n')
		for i in filter(lambda _i: _i[0].count('db'), api.iteritems()):
			func_decl('db',
			    i[0].split('.')[1], i[1][0], i[1][1:], tfile)

tfile.close()
compare_srcfile(tmp_file, '../inc_posix/wiredtiger.in')

os.remove(tmp_file)
