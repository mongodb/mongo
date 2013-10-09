# Output serialization functions.

import textwrap
from dist import compare_srcfile

class SerialArg:
	def __init__(self, typestr, name, sized=0):
		self.typestr = typestr
		self.name = name
		self.sized = sized

class Serial:
	def __init__(self, name, args):
		self.name = name
		self.args = args

msgtypes = [
Serial('col_append', [
		SerialArg('WT_PAGE *', 'page'),
		SerialArg('WT_INSERT_HEAD *', 'inshead'),
		SerialArg('WT_INSERT ***', 'ins_stack'),
		SerialArg('WT_INSERT **', 'next_stack'),
		SerialArg('WT_INSERT *', 'new_ins', 1),
		SerialArg('uint64_t *', 'recno'),
		SerialArg('u_int', 'skipdepth'),
	]),

Serial('insert', [
		SerialArg('WT_PAGE *', 'page'),
		SerialArg('WT_INSERT_HEAD *', 'inshead'),
		SerialArg('WT_INSERT ***', 'ins_stack'),
		SerialArg('WT_INSERT **', 'next_stack'),
		SerialArg('WT_INSERT *', 'new_ins', 1),
		SerialArg('u_int', 'skipdepth'),
	]),

Serial('update', [
		SerialArg('WT_PAGE *', 'page'),
		SerialArg('WT_UPDATE **', 'srch_upd'),
		SerialArg('WT_UPDATE *', 'old_upd'),
		SerialArg('WT_UPDATE *', 'upd', 1),
		SerialArg('WT_UPDATE **', 'upd_obsolete'),
	]),
]

# decl --
#	Return a declaration for the variable.
def decl(l):
	o = l.typestr
	if o[-1] != '*':
		o += ' '
	return o + l.name

# decl_p --
#	Return a declaration for a reference to the variable, which requires
# another level of indirection.
def decl_p(l):
	o = l.typestr
	if o[-1] != '*':
		o += ' '
	return o + '*' + l.name + 'p'

# output --
#	Create serialized function calls.
def output(entry, f):
	# structure declaration
	f.write('''
typedef struct {
''')
	sizes = 0;
	for l in entry.args:
		f.write('\t' + decl(l) + ';\n')
		if l.sized:
			sizes = 1
	f.write('} __wt_' + entry.name + '_args;\n\n')

	f.write('static inline int\n__wt_' + entry.name + '_serial(\n')
	o = 'WT_SESSION_IMPL *session'
	for l in entry.args:
		if l.sized:
			o += ', ' + decl_p(l) + ', size_t ' + l.name + '_size'
		else:
			o += ', ' + decl(l)
	o += ')'
	f.write('\n'.join('\t' + l for l in textwrap.wrap(o, 70)))
	f.write(' {\n')
	f.write('\t__wt_''' + entry.name + '_args _args, *args = &_args;\n')
	f.write('\tWT_DECL_RET;\n')
	if sizes:
		f.write('\tsize_t incr_mem;\n')
	f.write('\n')

	for l in entry.args:
		if l.sized:
			f.write('''\tif (''' + l.name + '''p == NULL)
\t\targs->''' + l.name + ''' = NULL;
\telse {
\t\targs->''' + l.name + ''' = *''' + l.name + '''p;
\t\t*''' + l.name + '''p = NULL;
\t}

''')
		else:
			f.write('\targs->' + l.name + ' = ' + l.name + ';\n\n')
	f.write('\t__wt_spin_lock(session, &S2BT(session)->serial_lock);\n')
	f.write('\tret = __wt_' + entry.name + '_serial_func(session, args);\n')
	f.write('\t__wt_spin_unlock(session, &S2BT(session)->serial_lock);\n')
	f.write('\n')

	f.write('\tif (ret != 0) {\n')
	if sizes:
		f.write('\t\t/* Free unused memory. */\n')
		for l in entry.args:
			if not l.sized:
				continue
			f.write(
			    '\t\t__wt_free(session, args->' + l.name + ');\n')
		f.write('''
\t\treturn (ret);
\t}
''')

	if sizes:
		f.write('''
\t/*
\t * Increment in-memory footprint after releasing the mutex: that's safe
\t * because the structures we added cannot be discarded while visible to
\t * any running transaction, and we're a running transaction, which means
\t * there can be no corresponding delete until we complete.
\t */
\tincr_mem = 0;
''')
		for l in entry.args:
			if not l.sized:
				continue
			f.write('\tWT_ASSERT(session, ' +
			    l.name + '_size != 0);\n')
			f.write('\tincr_mem += ' + l.name + '_size;\n')
		f.write('''\tif (incr_mem != 0)
\t\t__wt_cache_page_inmem_incr(session, page, incr_mem);

\t/* Mark the page dirty after updating the footprint. */
\t__wt_page_modify_set(session, page);

\treturn (0);
}

''')

	# unpack function
	f.write('static inline void\n__wt_' + entry.name + '_unpack(\n')
	o = 'void *untyped_args'
	for l in entry.args:
		o += ', ' + decl_p(l)
	o +=')'
	f.write('\n'.join('    ' + l for l in textwrap.wrap(o, 70)))
	f.write('''
{
\t__wt_''' + entry.name + '''_args *args = (__wt_''' + entry.name + '''_args *)untyped_args;

''')
	for l in entry.args:
		f.write('\t*' + l.name + 'p = args->' + l.name + ';\n')
	f.write('}\n')

#####################################################################
# Update serial_funcs.i.
#####################################################################
tmp_file = '__tmp'
tfile = open(tmp_file, 'w')
tfile.write('/* DO NOT EDIT: automatically built by dist/serial.py. */\n')

for entry in msgtypes:
	output(entry, tfile)

tfile.close()

compare_srcfile(tmp_file, '../src/include/serial_funcs.i')
