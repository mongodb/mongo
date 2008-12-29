# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$
#
# Read the api file and output C for the Db/Env structures, getter/setter
# functions, and other API initialization.

import os, re, string
from dist import compare_srcfile

# func_cast --
#	Function to initialize a method name to an error function.
def func_cast(rettype, handle, field, func, args):
	if field.count('get_'):
		redirect = '*'
	else:
		redirect = ''
	s = '\t' + handle + '->' + field +\
	    ' = (' + rettype + ' (*)\n\t    (' + handle.upper() + ' *'
	for l in args:
		s += ', ' + l.split('\t')[1].replace('@S', redirect)
	s += '))\n\t    __wt_' + handle + '_' + func + ';\n'
	return (s)

# handle_methods --
#	Standalone method code for the API.
def handle_methods():
	global db_config, db_header, db_lockout
	global env_config, env_header, env_lockout

	if condition.count('voidmethod'):
		rettype = 'void'
	else:
		rettype = 'int'
	field = list[0].split('\t')[0]

	# Store the handle's standalone methods into the XXX_header string. 
	s = '\t' +\
	    rettype + ' (*' + field + ')(\n\t    ' + handle.upper() + ' *, '
	for l in list:
		s += l.split('\t')[1].replace('@S', '*')
	s += ');\n'
	if handle.count('env'):
		env_header += s
	else:
		db_header += s

	# Store the initialization of the handle's standalone methods in the
	# XXX_config string.
	s = '\t' + handle +\
	    '->' + field + ' = __wt_' + handle + '_' + field + ';\n'
	if handle.count('env'):
		env_config += s
	else:
		db_config += s

	# Store the lockout of the handle's standalone methods in the
	# XXX_lockout string.  Note we skip the destroy method, it's
	# the only legal one.
	if not field.count('destroy'):
		s = func_cast(rettype, handle, field, 'lockout_err', list)
		if handle.count('env'):
			env_lockout += s
		else:
			db_lockout += s

# handle_getset --
#	Getter/setter code for the API.
def handle_getset():
	global db_config, db_header, db_lockout
	global env_config, env_header, env_lockout

	field = list[0].split('\t')[0]

	# Store the handle's getter/setter variables and methods into the
	# XXX_header string.
	s = ''
	for l in list:
		s += '\t' +\
		    l.split('\t')[1].replace('@S', l.split('\t')[0]) + ';\n'
	s += '\tvoid (*get_' + field + ')(\n\t    ' + handle.upper() + ' *'
	for l in list:
		s += ', ' + l.split('\t')[1].replace('@S', '*')
	s += ');\n'
	s += '\tint (*set_' + field + ')(\n\t    ' + handle.upper() + ' *'
	for l in list:
		s += ', ' + l.split('\t')[1].replace('@S', '')
	s += ');\n\n'
	if handle.count('env'):
		env_header += s
	else:
		db_header += s

	# Store the initialization of the handle's getter/setter methods in the
	# XXX_config string.
	s = '\t' + handle +\
	    '->get_' + field + ' = __wt_' + handle + '_get_' + field + ';\n'
	s += s.replace('get_', 'set_')
	if handle.count('env'):
		env_config += s
	else:
		db_config += s

	# Store the lockout of the handle's getter/setter methods in the
	# XXX_lockout string.
	s = func_cast('void', handle, 'get_' + field, 'lockout_err', list) +\
	    func_cast('int', handle, 'set_' + field, 'lockout_err', list)
	if handle.count('env'):
		env_lockout += s
	else:
		db_lockout += s

	# Output the getter function.
	s = 'static void __wt_' +\
	    handle + '_get_' + field + '(\n\t' + handle.upper() + ' *'
	for l in list:
		s += ',\n\t' + \
		    l.split('\t')[1].replace('@S', '*')
	s += ');\n'
	s += 'static void\n__wt_' +\
	    handle + '_get_' + field + '(\n\t' + handle.upper() + ' *handle'
	for l in list:
		s += ',\n\t' +\
		    l.split('\t')[1].replace('@S', '*' + l.split('\t')[0] + 'p')
	s += ')\n{\n'
	for l in list:
		s += '\t*' + l.split('\t')[0] + 'p = ' +\
		    'handle->' + l.split('\t')[0] + ';\n'
	s += '}\n\n'
	tfile.write(s)

	# Output the setter function.
	s = 'static int __wt_' +\
	    handle + '_set_' + field + '(\n\t' + handle.upper() + ' *'
	for l in list:
		s += ',\n\t' + l.split('\t')[1].replace('@S', '')
	s += ');\n'
	s += 'static int\n__wt_' +\
	    handle + '_set_' + field + '(\n\t' + handle.upper() + ' *handle'
	for l in list:
		s += ',\n\t' + l.split('\t')[1].replace('@S', l.split('\t')[0])
	s += ')\n{\n'

	# Verify means we call a standard verification routine because
	# there are constraints or side-effects on setting the value.
	if condition.count('verify'):
		s += '\tint ret;\n\n'
		s += '\tif ((ret = __wt_' +\
		    handle + '_set_' + field + '_verify(\n\t    handle'
		for l in list:
			s += ', &' + l.split('\t')[0]
		s += ')) != 0)\n\t\treturn (ret);\n\n'

	for l in list:
		s += '\thandle->' +\
		    l.split('\t')[0] + ' = ' + l.split('\t')[0] + ';\n'
	s += '\treturn (0);\n}\n\n'
	tfile.write(s)

db_config = ''					# Env method init
db_header = ''					# Db handle structure
db_lockout = ''					# Db lockout function
env_config = ''					# Db method init
env_header = ''					# Env handle structure
env_lockout = ''				# Env lockout function

tmp_file = '__tmp'
tfile = open(tmp_file, 'w')
tfile.write('/* DO NOT EDIT: automatically built by dist/api.py. */\n\n')
tfile.write('#include "wt_internal.h"\n\n')

setter_re = re.compile(r'^[a-z]')
field_re = re.compile(r'^\t[a-z]')
list = []
for line in open('api', 'r'):
	if setter_re.match(line):
		if list:
			if condition.count('getset'):
				handle_getset()
			if condition.count('method'):
				handle_methods()
			list=[]
		s = line.split('\t')
		handle = s[0]
		condition = s[1]
	elif field_re.match(line):
		list.append(string.strip(line))
if list:
	if condition.count('getset'):
		handle_getset()
	if condition.count('method'):
		handle_methods()

# Write out the configuration initialization and lockout functions.
tfile.write('void\n__wt_env_config_methods(ENV *env)\n{\n');
tfile.write(env_config)
tfile.write('}\n\n')
tfile.write('void\n__wt_env_config_methods_lockout(ENV *env)\n{\n');
tfile.write(env_lockout)
tfile.write('}\n\n')
tfile.write('void\n__wt_db_config_methods(DB *db)\n{\n');
tfile.write(db_config)
tfile.write('}\n\n')
tfile.write('void\n__wt_db_config_methods_lockout(DB *db)\n{\n');
tfile.write(db_lockout)
tfile.write('}\n')
	
# Update the automatically generated function sources.
tfile.close()
compare_srcfile(tmp_file, '../support/getset.c')

# Update the wiredtiger.in file with Env and Db handle information.
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
		tfile.write(env_header)
	elif line.count('Db handle api section: BEGIN'):
		skip = 1
		tfile.write('\t */\n')
		tfile.write(db_header)
tfile.close()
compare_srcfile(tmp_file, '../inc_posix/wiredtiger.in')

os.remove(tmp_file)
