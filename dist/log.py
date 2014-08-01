#!/usr/bin/env python

import os, re, sys, textwrap
from dist import compare_srcfile
import log_data

# Temporary file.
tmp_file = '__tmp'

# Map log record types to:
# (C type, pack type, printf format, printf arg(s))
field_types = {
	'string' : ('const char *', 'S', '%s', 'arg'),
	'item' : ('WT_ITEM *', 'u', '%.*s',
	    '(int)arg.size, (const char *)arg.data'),
	'recno' : ('uint64_t', 'r', '%" PRIu64 "', 'arg'),
	'uint32' : ('uint32_t', 'I', '%" PRIu32 "', 'arg'),
	'uint64' : ('uint64_t', 'Q', '%" PRIu64 "', 'arg'),
}

def cintype(f):
	return field_types[f[0]][0]

def couttype(f):
	type = cintype(f)
	# We already have a pointer to a WT_ITEM
	if f[0] == 'item':
		return type
	if type[-1] != '*':
		type += ' '
	return type + '*'

def clocaltype(f):
	type = cintype(f)
	# Allocate a WT_ITEM struct on the stack
	if f[0] == 'item':
		return type[:-2]
	return type

def pack_fmt(fields):
	return ''.join(field_types[f[0]][1] for f in fields)

def op_pack_fmt(r):
	return 'II' + pack_fmt(r.fields)

def rec_pack_fmt(r):
	return 'I' + pack_fmt(r.fields)

def printf_fmt(f):
	return field_types[f[0]][2]

def printf_arg(f):
	arg = field_types[f[0]][3].replace('arg', f[1])
	return '\n\t    ' + arg if f[0] == 'item' else ' ' + arg

#####################################################################
# Update log.h with #defines for types
#####################################################################
log_defines = (
	''.join('/*! %s */\n#define\t%s\t%d\n' % (r.desc, r.macro_name(), i)
		for i, r in enumerate(log_data.rectypes)) +
	''.join('/*! %s */\n#define\t%s\t%d\n' % (r.desc, r.macro_name(), i)
		for i, r in enumerate(log_data.optypes,start=1))
)

tfile = open(tmp_file, 'w')
skip = 0
for line in open('../src/include/wiredtiger.in', 'r'):
	if skip:
		if 'Log record declarations: END' in line:
			tfile.write('/*\n' + line)
			skip = 0
	else:
		tfile.write(line)
	if 'Log record declarations: BEGIN' in line:
		skip = 1
		tfile.write(' */\n')
		tfile.write('/*! invalid operation */\n')
		tfile.write('#define\tWT_LOGOP_INVALID\t0\n')
		tfile.write(log_defines)
tfile.close()
compare_srcfile(tmp_file, '../src/include/wiredtiger.in')

#####################################################################
# Create log_auto.c with handlers for each record / operation type.
#####################################################################
f='../src/log/log_auto.c'
tfile = open(tmp_file, 'w')

tfile.write('/* DO NOT EDIT: automatically built by dist/log.py. */\n')

tfile.write('''
#include "wt_internal.h"

int
__wt_logrec_alloc(WT_SESSION_IMPL *session, size_t size, WT_ITEM **logrecp)
{
	WT_ITEM *logrec;

	WT_RET(__wt_scr_alloc(session, WT_ALIGN(size + 1, LOG_ALIGN), &logrec));
	WT_CLEAR(*(WT_LOG_RECORD *)logrec->data);
	logrec->size = offsetof(WT_LOG_RECORD, record);

	*logrecp = logrec;
	return (0);
}

void
__wt_logrec_free(WT_SESSION_IMPL *session, WT_ITEM **logrecp)
{
	WT_UNUSED(session);
	__wt_scr_free(logrecp);
}

int
__wt_logrec_read(WT_SESSION_IMPL *session,
    const uint8_t **pp, const uint8_t *end, uint32_t *rectypep)
{
	uint64_t rectype;

	WT_UNUSED(session);
	WT_RET(__wt_vunpack_uint(pp, WT_PTRDIFF(end, *pp), &rectype));
	*rectypep = (uint32_t)rectype;
	return (0);
}

int
__wt_logop_read(WT_SESSION_IMPL *session,
    const uint8_t **pp, const uint8_t *end,
    uint32_t *optypep, uint32_t *opsizep)
{
	return (__wt_struct_unpack(
	    session, *pp, WT_PTRDIFF(end, *pp), "II", optypep, opsizep));
}
''')

# Emit code to read, write and print log operations (within a log record)
for optype in log_data.optypes:
	if not optype.fields:
		continue

	tfile.write('''
int
__wt_logop_%(name)s_pack(
    WT_SESSION_IMPL *session, WT_ITEM *logrec,
    %(arg_decls)s)
{
	const char *fmt = WT_UNCHECKED_STRING(%(fmt)s);
	size_t size;
	uint32_t optype, recsize;

	optype = %(macro)s;
	WT_RET(__wt_struct_size(session, &size, fmt,
	    optype, 0%(arg_names)s));

	__wt_struct_size_adjust(session, &size);
	WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
	recsize = (uint32_t)size;
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    optype, recsize%(arg_names)s));

	logrec->size += (uint32_t)size;
	return (0);
}
''' % {
	'name' : optype.name,
	'macro' : optype.macro_name(),
	'arg_decls' : ', '.join(
	    '%s%s%s' % (cintype(f), '' if cintype(f)[-1] == '*' else ' ', f[1])
	    for f in optype.fields),
	'arg_names' : ''.join(', %s' % f[1] for f in optype.fields),
	'fmt' : op_pack_fmt(optype)
})

	tfile.write('''
int
__wt_logop_%(name)s_unpack(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
    %(arg_decls)s)
{
	const char *fmt = WT_UNCHECKED_STRING(%(fmt)s);
	uint32_t optype, size;

	WT_RET(__wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), fmt,
	    &optype, &size%(arg_names)s));
	WT_ASSERT(session, optype == %(macro)s);

	*pp += size;
	return (0);
}
''' % {
	'name' : optype.name,
	'macro' : optype.macro_name(),
	'arg_decls' : ', '.join(
	    '%s%sp' % (couttype(f), f[1]) for f in optype.fields),
	'arg_names' : ''.join(', %sp' % f[1] for f in optype.fields),
	'fmt' : op_pack_fmt(optype)
})

	tfile.write('''
int
__wt_logop_%(name)s_print(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, FILE *out)
{
	%(arg_decls)s

	WT_RET(__wt_logop_%(name)s_unpack(
	    session, pp, end%(arg_addrs)s));

	fprintf(out, "    \\"optype\\": \\"%(name)s\\",\\n");
	%(print_args)s
	return (0);
}
''' % {
	'name' : optype.name,
	'arg_decls' : '\n\t'.join('%s%s%s;' %
	    (clocaltype(f), '' if clocaltype(f)[-1] == '*' else ' ', f[1])
	    for f in optype.fields),
	'arg_addrs' : ''.join(', &%s' % f[1] for f in optype.fields),
	'print_args' : '\n\t'.join(
	    'fprintf(out, "    \\"%s\\": \\"%s\\",\\n",%s);' %
	    (f[1], printf_fmt(f), printf_arg(f))
	    for f in optype.fields),
})

# Emit the printlog entry point
tfile.write('''
int
__wt_txn_op_printlog(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, FILE *out)
{
	uint32_t optype, opsize;

	/* Peek at the size and the type. */
	WT_RET(__wt_logop_read(session, pp, end, &optype, &opsize));
	end = *pp + opsize;

	switch (optype) {''')

for optype in log_data.optypes:
	if not optype.fields:
		continue

	tfile.write('''
	case %(macro)s:
		WT_RET(%(print_func)s(session, pp, end, out));
		break;
''' % {
	'macro' : optype.macro_name(),
	'print_func' : '__wt_logop_' + optype.name + '_print',
})

tfile.write('''
	WT_ILLEGAL_VALUE(session);
	}

	return (0);
}
''')

tfile.close()
compare_srcfile(tmp_file, f)
