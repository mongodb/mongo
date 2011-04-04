# Read the api file and output C for the SESSION/BTREE structures, getter/setter
# functions, and other API initialization.

import string, sys
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

# func_method_init --
#     Set getter/setter initialization values.
def func_method_init(handle, decl, f):
	f.write('void\n__wt_methods_' +
	    handle + '_config_default(' + decl + ')\n{\n')
	for i in sorted(filter(lambda _i:
	    _i[1].handle == handle, api.items())):
		for l in i[1].args:
			if l.count('/') == 2:
				f.write('\t' + handle + '->' +
				  l.split('/')[0] + ' = ' +
				  l.split('/')[2] + ';\n')
	f.write('}\n\n')

# func_method_lockout --
#	Set a handle's methods to the lockout function (skipping the close
#	method, it's always legal).
def func_method_lockout(handle, decl, f):
	quiet = 1
	f.write('void\n__wt_methods_' + handle + '_lockout(' + decl + ')\n{\n')
	for i in sorted(filter(lambda _i:
	    _i[1].handle == handle and _i[1].method != 'close',
	    api.items())):
		func_method_single(i[1].handle,
		    i[1].method, i[1].config, i[1].args, 'lockout', f)
		quiet = 0;
	if quiet:
		f.write('\tWT_UNUSED(' + handle + ');\n')
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
	    _i[1].handle == handle, api.items())):
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
	    _i[1].handle.count(handle), api.items())):
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
	    _i[1].config.count('extfunc'), api.items())):
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

	extfunc = config.count('extfunc')

	# Declarations:
	# If we don't have a connection handle, acquire one.
	if handle != 'connection':
		if handle == 'session':
			f.write('\tCONNECTION *connection = S2C(' + handle + ');\n')
		else:
			f.write('\tCONNECTION *connection = ' + handle + '->conn;\n')
	if handle != 'session':
		f.write('\tSESSION *session = &connection->default_session;\n')
	# If we are hand-coding the routine, we'll need a place to save the
	# return value.
	if extfunc:
		f.write('\tint ret;\n')
	if handle != 'connection' or extfunc:
		f.write('\n')

	# If we have a "flags" argument to a setter function, check it
	# before we continue.
	if config.count('setter'):
		for l in args:
			if l.count('flags/'):
				f.write('\tWT_CONN_FCHK(connection, "' +
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
		    handle + '_' + method + '_verify(\n\t    ' + handle)
		s = ''
		for l in args:
			s += ', ' + l.split('/')[0]
		s += ')'
		f.write(s + '));\n')

	# getter/setter implies connlock: lock the data structure.
	f.write('\t__wt_lock(session, connection->mtx);\n')

	# Count the call.
	s = a.handle + '_' + a.method
	f.write('\tWT_STAT_INCR(connection->method_stats, ' + s.upper() + ');\n')

	# If the function is hand-coded, just call it.
	if extfunc:
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
			if l.count('flags/'):
				if flags[a.key][0] != '__NONE__':
					f.write('\tF_SET(' + handle + ', flags);\n')
				continue
			f.write('\t' + handle + '->' +
			    l.split('/')[0] + ' = ' + l.split('/')[0] + ';\n')

	# getter/setter implies connlock: unlock the data structure.
	f.write('\t__wt_unlock(session, connection->mtx);\n')
	f.write('\treturn (')
	if extfunc:
		f.write('ret')
	else:
		f.write('0')
	f.write(');\n}\n\n')

# func_method --
#	Generate all non-getter/setter API entry functions.
def func_method(a, f):
	# Write the declaration for this function.
	func_method_decl(a, f)

	handle = a.handle
	method = a.method
	config = a.config
	args = a.args

	colonly = config.count('colonly')	# Check row-only databases
	locking = config.count('connlock')	# Lock
	rdonly = config.count('rdonly')		# Check read-only databases
	restart = config.count('restart')	# Handle WT_RESTART
	rowonly = config.count('rowonly')	# Check row-only databases

	flagchk = 0				# We're checking flags
	for l in args:
		if l.count('flags/'):
			flagchk = 1

	session = config.count('session')	# Handle a SESSION
	have_session = (handle == 'session')
	for arg in args:
		have_session = have_session or arg.startswith('session/')

	# We may need a method name.
	if colonly or flagchk or rdonly or rowonly or session:
		f.write('\tconst char *method_name = "' +
		    handle.upper() + '.' + method + '";\n')

	# We need a CONNECTION handle, find it.
	if handle != 'connection':
		if handle == 'session':
			f.write('\tCONNECTION *connection = S2C(' + handle + ');\n')
		else:
			f.write('\tCONNECTION *connection = ' + handle + '->conn;\n')

	# If we're allocating a SESSION, we'll need a pointer on the stack.
	if session and not have_session:
		f.write('\tSESSION *session = NULL;\n')
	elif locking and not have_session:
		f.write('\tSESSION *session = &connection->default_session;\n')
	f.write('\tint ret;\n\n')

	# Check if the method is illegal for the database type.
	if colonly:
		f.write('\tWT_DB_COL_ONLY(session, btree, method_name);\n')
	if rowonly:
		f.write('\tWT_DB_ROW_ONLY(session, btree, method_name);\n')

	# Check if the method is illegal for read-only databases.
	if rdonly:
		f.write('\tWT_DB_RDONLY(session, btree, method_name);\n')

	# If entering the API with a SESSION handle, allocate/initialize it.
	if session:
		f.write('\tWT_RET(' +
		    '__wt_session_api_set(connection, method_name, btree, &session));\n')

	# Check flags.
	if flagchk:
		f.write('\tWT_CONN_FCHK(connection, method_name, flags, WT_APIMASK_' +
		    handle.upper() + '_' + method.upper() + ');\n')

	# Acquire the lock.
	if locking:
		f.write('\t__wt_lock(session, connection->mtx);\n')

	# Method call statistics -- we put the update inside the lock just to
	# make the stat value a little more precise.
	stat = a.handle + '_' + a.method
	stat = stat.upper()
	f.write('\tWT_STAT_INCR(connection->method_stats, ' + stat + ');\n')

	# Call the underlying method function, handling restart as necessary.
	f.write('\t')
	if restart:
		f.write('while ((')
	f.write('ret = __wt_' + handle + '_' + method + '(')

	# Some functions take BTREE handles but pass SESSION handles; some functions
    # take BTREE and SESSION handles, and pass SESSION handles.
	if session:
		handle = 'session'
	f.write(handle)
	for l in args:
		if l.count('flags/') and flags[a.key][0] == '__NONE__':
			continue
		if not session or not l.count('session/'):
			f.write(', ' + l.split('/')[0])
	if restart:
		f.write(')) == WT_RESTART)\n')
		f.write('\t\tWT_STAT_INCR(connection->method_stats, ' +
		    stat + '_RESTART);\n')
	else:
		f.write(');\n')

	# Unlock.
	if locking:
		if handle == 'session' and method == 'close':
			f.write('\tsession = &connection->default_session;\n')
		f.write('\t__wt_unlock(session, connection->mtx);\n')

	# If entering the API with a SESSION handle, free/clear it.
	if session:
		f.write('\tWT_TRET(__wt_session_api_clr(session, method_name, ')
		if not have_session:
			f.write('1')
		else:
			f.write('0')
		f.write('));\n')

	# And return.
	f.write('\treturn (ret);\n}\n\n')

#####################################################################
# Build the API dictionary.
#####################################################################
import api_class
api = api_class.methods
flags = api_class.flags

#####################################################################
# Update api_int.c, the API source file.
#####################################################################
tfile = open(tmp_file, 'w')
tfile.write('/* DO NOT EDIT: automatically built by dist/api.py. */\n\n')
tfile.write('#include "wt_internal.h"\n\n')

# We need an API function for any method not marked 'noauto'.
for i in sorted(api.items()):
	if i[1].config.count('noauto'):
		continue
	if i[1].config.count('getter') or i[1].config.count('setter'):
		func_method_getset(i[1], tfile)
	else:
		func_method(i[1], tfile)

# Write the method default initialization, lockout and transition functions.
func_method_init('btree', 'BTREE *btree', tfile)
func_method_lockout('btree', 'BTREE *btree', tfile)
func_method_transition('btree', 'BTREE *btree', tfile)

func_method_init('connection', 'CONNECTION *connection', tfile)
func_method_lockout('connection', 'CONNECTION *connection', tfile)
func_method_transition('connection', 'CONNECTION *connection', tfile)

func_method_lockout('session', 'SESSION *session', tfile)
func_method_transition('session', 'SESSION *session', tfile)

tfile.close()
compare_srcfile(tmp_file, '../src/api/api_int.c')

#####################################################################
# Update api.h file with SESSION/BTREE methods and BTREE/CONNECTION
# getter/setter variables.
#####################################################################
tfile = open(tmp_file, 'w')
skip = 0
for line in open('../src/include/api.h', 'r'):
	if skip:
		if line.count('BTREE methods: END') or\
		    line.count('CONNECTION methods: END') or\
		    line.count('SESSION methods: END') or\
		    line.count('SESSION getter/setter variables: END') or\
		    line.count('BTREE getter/setter variables: END') or\
		    line.count('CONNECTION getter/setter variables: END'):
			tfile.write('\t/*\n' + line)
			skip = 0
	else:
		tfile.write(line)
	if line.count('BTREE methods: BEGIN'):
		skip = 1
		tfile.write('\t */')
		func_struct_all('btree', tfile)
	elif line.count('CONNECTION methods: BEGIN'):
		skip = 1
		tfile.write('\t */')
		func_struct_all('connection', tfile)
	elif line.count('SESSION methods: BEGIN'):
		skip = 1
		tfile.write('\t */')
		func_struct_all('session', tfile)
	elif line.count('BTREE getter/setter variables: BEGIN'):
		skip = 1
		tfile.write('\t */')
		func_struct_variable_all('btree', tfile)
	elif line.count('CONNECTION getter/setter variables: BEGIN'):
		skip = 1
		tfile.write('\t */')
		func_struct_variable_all('connection', tfile)
	elif line.count('SESSION getter/setter variables: BEGIN'):
		skip = 1
		tfile.write('\t */')
		func_struct_variable_all('session', tfile)

tfile.close()
compare_srcfile(tmp_file, '../src/include/api.h')
