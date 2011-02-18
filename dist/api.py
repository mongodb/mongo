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
	# If we don't have an environment handle, acquire one.
	# If we are hand-coding the routine, we'll need a place to save the
	# return value.
	if handle != 'env':
		f.write('\tENV *env = ' + handle + '->env;\n')
	f.write('\tIENV *ienv = env->ienv;\n')
	if extfunc:
		f.write('\tint ret;\n')
	if handle != 'env' or extfunc:
		f.write('\n')

	# If we have a "flags" argument to a setter function, check it
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
		    handle + '_' + method + '_verify(\n\t    ' + handle)
		s = ''
		for l in args:
			s += ', ' + l.split('/')[0]
		s += ')'
		f.write(s + '));\n')

	# getter/setter implies ienvlock: lock the data structure.
	f.write('\t__wt_lock(env, ienv->mtx);\n')

	# Count the call.
	s = a.handle + '_' + a.method
	f.write('\tWT_STAT_INCR(ienv->method_stats, ' + s.upper() + ');\n')

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
			if l.count('flags/') and flags[a.key][0] == '__NONE__':
				continue
			f.write('\t' + handle + '->' +
			    l.split('/')[0] + ' = ' + l.split('/')[0] + ';\n')

	# getter/setter implies ienvlock: unlock the data structure.
	f.write('\t__wt_unlock(env, ienv->mtx);\n')
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
	locking = config.count('ienvlock')	# Lock
	rdonly = config.count('rdonly')		# Check read-only databases
	restart = config.count('restart')	# Handle WT_RESTART
	rowonly = config.count('rowonly')	# Check row-only databases

	toc = config.count('toc')		# Handle a WT_TOC
	toc_alloc = toc and not args[0].count('toc/')

	flagchk = 0				# We're checking flags
	for l in args:
		if l.count('flags/'):
			flagchk = 1

	# We may need a method name.
	if colonly or flagchk or rdonly or rowonly or toc:
		f.write('\tconst char *method_name = "' +
		    handle.upper() + '.' + method + '";\n')

	# We need an ENV/IENV handle pair, find them.
	if (flagchk or locking) and handle != 'env':
		f.write('\tENV *env = ' + handle + '->env;\n')
	f.write('\tIENV *ienv = env->ienv;\n')

	# If we're allocating a WT_TOC, we'll need a pointer on the stack.
	if toc_alloc:
		f.write('\tWT_TOC *toc = NULL;\n')
	f.write('\tint ret;\n\n')

	# Check if the method is illegal for the database type.
	if colonly:
		f.write('\tWT_DB_COL_ONLY(db, method_name);\n')
	if rowonly:
		f.write('\tWT_DB_ROW_ONLY(db, method_name);\n')

	# Check if the method is illegal for read-only databases.
	if rdonly:
		f.write('\tWT_DB_RDONLY(db, method_name);\n')

	# Check flags.
	if flagchk:
		f.write('\tWT_ENV_FCHK(env, method_name, flags, WT_APIMASK_' +
		    handle.upper() + '_' + method.upper() + ');\n')

	# If entering the API with a WT_TOC handle, allocate/initialize it.
	if toc:
		f.write('\tWT_RET(' +
		    '__wt_toc_api_set(env, method_name, db, &toc));\n')

	# Acquire the lock.
	if locking:
		f.write('\t__wt_lock(env, ienv->mtx);\n')

	# Method call statistics -- we put the update inside the lock just to
	# make the stat value a little more precise.
	stat = a.handle + '_' + a.method
	stat = stat.upper()
	f.write('\tWT_STAT_INCR(ienv->method_stats, ' + stat + ');\n')

	# Call the underlying method function, handling restart as necessary.
	f.write('\t')
	if restart:
		f.write('while ((')
	f.write('ret = __wt_' + handle + '_' + method + '(')

	# Some functions take DB handles but pass WT_TOC handles; some functions	# take DB and WT_TOC handles, and pass WT_TOC handles.
	if toc:
		handle = 'toc'
	f.write(handle)
	for l in args:
		if l.count('flags/') and flags[a.key][0] == '__NONE__':
			continue
		if not toc or not l.count('toc/'):
			f.write(', ' + l.split('/')[0])
	if restart:
		f.write(')) == WT_RESTART)\n')
		f.write('\t\tWT_STAT_INCR(ienv->method_stats, ' +
		    stat + '_RESTART);\n')
	else:
		f.write(');\n')

	# Unlock.
	if locking:
		f.write('\t__wt_unlock(env, ienv->mtx);\n')

	# If entering the API with a WT_TOC handle, free/clear it.
	if toc:
		f.write('\tWT_TRET(__wt_toc_api_clr(toc, method_name, ')
		if toc_alloc:
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
func_method_init('db', 'DB *db', tfile)
func_method_lockout('db', 'DB *db', tfile)
func_method_transition('db', 'DB *db', tfile)

func_method_init('env', 'ENV *env', tfile)
func_method_lockout('env', 'ENV *env', tfile)
func_method_transition('env', 'ENV *env', tfile)

func_method_lockout('wt_toc', 'WT_TOC *wt_toc', tfile)
func_method_transition('wt_toc', 'WT_TOC *wt_toc', tfile)

tfile.close()
compare_srcfile(tmp_file, '../src/support/api_int.c')

#####################################################################
# Update wiredtiger.in file with WT_TOC/DB methods and DB/ENV getter/setter
# variables.
#####################################################################
tfile = open(tmp_file, 'w')
skip = 0
for line in open('../src/include/wiredtiger.in', 'r'):
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
compare_srcfile(tmp_file, '../src/include/wiredtiger.in')

os.remove(tmp_file)
