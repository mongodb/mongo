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
		SerialArg('WT_INSERT_HEAD *', 'ins_head'),
		SerialArg('WT_INSERT ***', 'ins_stack'),
		SerialArg('WT_INSERT *', 'new_ins', 1),
		SerialArg('uint64_t *', 'recnop'),
		SerialArg('u_int', 'skipdepth'),
	]),

Serial('insert', [
		SerialArg('WT_PAGE *', 'page'),
		SerialArg('WT_INSERT_HEAD *', 'ins_head'),
		SerialArg('WT_INSERT ***', 'ins_stack'),
		SerialArg('WT_INSERT *', 'new_ins', 1),
		SerialArg('u_int', 'skipdepth'),
	]),

Serial('update', [
		SerialArg('WT_PAGE *', 'page'),
		SerialArg('WT_UPDATE **', 'srch_upd'),
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
	# Function declaration.
	f.write('static inline int\n__wt_' + entry.name + '_serial(\n')
	o = 'WT_SESSION_IMPL *session'
	for l in entry.args:
		if l.sized:
			o += ', ' + decl_p(l) + ', size_t ' + l.name + '_size'
		else:
			o += ', ' + decl(l)
	o += ')'
	f.write('\n'.join('\t' + l for l in textwrap.wrap(o, 70)))
	f.write('\n{')

	# Local variable declarations.
	for l in entry.args:
		if l.sized:
			f.write('''
\t''' + decl(l) + ''' = *''' + l.name + '''p;
\tWT_DECL_RET;
\tsize_t incr_mem;
''')

	# Clear memory references we now own.
	for l in entry.args:
		if l.sized:
			f.write('''
\t/* Clear references to memory we now own. */
\t*''' + l.name + '''p = NULL;
''')

	f.write('''
\t/* Acquire the serialization spinlock, call the worker function. */
\t__wt_spin_lock(session, &S2BT(session)->serial_lock);
\tret = __''' + entry.name + '''_serial_func(
''')

	o = 'session'
	for l in entry.args:
		o += ', ' + l.name
	o += ');'
	f.write('\n'.join('\t    ' + l for l in textwrap.wrap(o, 70)))
	f.write('''
\t__wt_spin_unlock(session, &S2BT(session)->serial_lock);
''')

	f.write('''
\t/* Free unused memory on error. */
\tif (ret != 0) {
''')
	for l in entry.args:
		if not l.sized:
			continue
		f.write(
		    '\t\t__wt_free(session, ' + l.name + ');\n')
	f.write('''
\t\treturn (ret);
\t}
''')

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

#####################################################################
# Update serial.i.
#####################################################################
tmp_file = '__tmp'
tfile = open(tmp_file, 'w')
skip = 0
for line in open('../src/include/serial.i', 'r'):
	if not skip:
		tfile.write(line)
	if line.count('Serialization function section: END'):
		tfile.write(line)
		skip = 0
	elif line.count('Serialization function section: BEGIN'):
		tfile.write(' */\n\n')
		skip = 1

		for entry in msgtypes:
			output(entry, tfile)

		tfile.write('/*\n')

tfile.close()
compare_srcfile(tmp_file, '../src/include/serial.i')
