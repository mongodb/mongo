# Output serialization functions.

import textwrap
from dist import compare_srcfile

class Serial:
	def __init__(self, key, op, spin, args):
		self.key = key
		self.op = op
		self.spin = spin
		self.args = args

serial = {
'cache_read' : Serial(
	'cache_read',
	'WT_WORKQ_READ',
	0, [
		[ 'WT_PAGE *', 'parent', 0 ],
		[ 'WT_REF *', 'parent_ref', 0 ],
		[ 'int', 'dsk_verify', 0 ]
	 ]),

'col_extend' : Serial(
	'col_extend',
	'WT_WORKQ_FUNC',
	1, [
		[ 'WT_PAGE *', 'page', 0 ],
		[ 'WT_PAGE *', 'new_intl', 1 ],
		[ 'WT_COL_REF *', 't', 1 ],
		[ 'uint32_t', 'internal_extend', 0 ],
		[ 'WT_PAGE *', 'new_leaf', 1 ],
		[ 'WT_COL *', 'd', 1 ],
		[ 'uint32_t', 'leaf_extend', 0 ],
		[ 'uint64_t', 'recno', 0 ],
	]),

'evict_file' : Serial(
	'evict_file',
	'WT_WORKQ_EVICT',
	0, [
		[ 'int', 'close_method', 0 ]
	]),

'insert' : Serial(
	'insert',
	'WT_WORKQ_FUNC',
	1, [
		[ 'WT_PAGE *', 'page', 0 ],
		[ 'uint32_t', 'write_gen', 0 ],
		[ 'WT_INSERT **', 'new_ins', 1 ],
		[ 'WT_INSERT **', 'srch_ins', 0 ],
		[ 'WT_INSERT *', 'ins', 1 ]
	]),

'row_key' : Serial(
	'row_key',
	'WT_WORKQ_FUNC',
	1, [
		[ 'WT_PAGE *', 'page', 0 ],
		[ 'WT_ROW *', 'row_arg', 0 ],
		[ 'WT_IKEY *', 'ikey', 0 ]
	]),

'update' : Serial(
	'update',
	'WT_WORKQ_FUNC',
	1, [
		[ 'WT_PAGE *', 'page', 0 ],
		[ 'uint32_t', 'write_gen', 0 ],
		[ 'WT_UPDATE **', 'new_upd', 1 ],
		[ 'WT_UPDATE **', 'srch_upd', 0 ],
		[ 'WT_UPDATE *', 'upd', 1 ]
	])
}

# decl --
#	Return a declaration for the variable.
def decl(l):
	o = l[0]
	if l[0][len(l[0]) - 1] != '*':
		o += ' '
	return o + l[1]

# decl_p --
#	Return a declaration for a reference to the variable, which requires
# another level of indirection.
def decl_p(l):
	o = l[0]
	if l[0][len(l[0]) - 1] != '*':
		o += ' '
	return o + '*' + l[1] + 'p'

# output --
#	Create functions to schedule tasks for the workQ thread.
def output(entry, f):
	# structure declaration
	f.write('\n')
	f.write('typedef struct {\n')
	for l in entry[1].args:
		f.write('\t' + decl(l) + ';\n')
		if l[2]:
			f.write('\tuint32_t ' + l[1] + '_size;\n')
			f.write('\tint ' + l[1] + '_taken;\n')
	f.write('} __wt_' + entry[0] + '_args;\n\n')

	# pack function
	f.write('static inline int\n__wt_' + entry[0] + '_serial(\n')
	o = 'WT_SESSION_IMPL *session'
	for l in entry[1].args:
		if l[2]:
			o += ', ' + decl_p(l) + ', uint32_t ' + l[1] + '_size'
		else:
			o += ', ' + decl(l)
	o += ')'
	for l in textwrap.wrap(o, 70):
		f.write('\t' + l + '\n')
	f.write('{\n')
	f.write('\t__wt_' + entry[0] + '_args _args, *args = &_args;\n')
	f.write('\tint ret;\n\n')
	for l in entry[1].args:
		if l[2]:
			f.write('\tif (' + l[1] + 'p == NULL)\n')
			f.write('\t\targs->' + l[1] + ' = NULL;\n')
			f.write('\telse {\n')
			f.write('\t\targs->' + l[1] + ' = *' + l[1] + 'p;\n')
			f.write('\t\t*' + l[1] + 'p = NULL;\n')
			f.write('\t\targs->' +
			    l[1] + '_size = ' + l[1] + '_size;\n')
			f.write('\t}\n')
			f.write('\targs->' + l[1] + '_taken = 0;\n\n')
		else:
			f.write('\targs->' + l[1] + ' = ' + l[1] + ';\n\n')
	f.write('\tret = __wt_session_serialize_func(session,\n')
	f.write('\t    ' + entry[1].op + ', ' + str(entry[1].spin) +
	    ', __wt_' + entry[1].key + '_serial_func, args);\n\n')
	for l in entry[1].args:
		if l[2]:
			f.write('\tif (!args->' + l[1] + '_taken)\n')
			f.write(
			    '\t\t__wt_free(session, args->' + l[1] + ');\n')
	f.write('\treturn (ret);\n')
	f.write('}\n\n')

	# unpack function
	f.write('static inline void\n__wt_' + entry[0] + '_unpack(\n')
	o = 'WT_SESSION_IMPL *session'
	for l in entry[1].args:
		o += ', ' + decl_p(l)
	o +=')'
	for l in textwrap.wrap(o, 70):
		f.write('\t' + l + '\n')
	f.write('{\n')
	f.write('\t__wt_' + entry[0] + '_args *args =\n	    ')
	f.write('(__wt_' + entry[0] + '_args *)session->wq_args;\n\n')
	for l in entry[1].args:
		f.write('\t*' + l[1] + 'p = args->' + l[1] + ';\n')
	f.write('}\n')

	# taken functions
	for l in entry[1].args:
		if l[2]:
			f.write('\n')
			f.write('static inline void\n__wt_' + entry[0] +
			    '_' + l[1] +
			    '_taken(WT_SESSION_IMPL *session, WT_PAGE *page)\n')
			f.write('{\n')
			f.write('\t__wt_' +
			    entry[0] + '_args *args =\n	    ')
			f.write('(__wt_' +
			    entry[0] + '_args *)session->wq_args;\n\n')
			f.write('\targs->' + l[1] + '_taken = 1;\n\n')
			f.write('\tWT_ASSERT(session, args->' +
			    l[1] + '_size != 0);\n')
			f.write('\t__wt_cache_page_workq_incr(' +
			    'session, page, args->' + l[1] + '_size);\n')
			f.write('}\n')

#####################################################################
# Update serial.i.
#####################################################################
tmp_file = '__tmp'
tfile = open(tmp_file, 'w')
tfile.write('/* DO NOT EDIT: automatically built by dist/serial.py. */\n')

for entry in sorted(serial.items()):
	output(entry, tfile)

tfile.close()

compare_srcfile(tmp_file, '../src/include/serial.i')
