#!/usr/bin/env python

import os, re, sys, textwrap
from dist import compare_srcfile
import log_data

# Temporary file.
tmp_file = '__tmp'

# Map log record types to (C input type, C output type, pack type)
field_types = {
		'string' : ('const char *', 'const char **', 'S'),
		'item' : ('WT_ITEM *', 'WT_ITEM *', 'u'),
		'recno' : ('uint64_t', 'uint64_t *', 'r'),
		'uint32' : ('uint32_t', 'uint32_t *', 'I'),
		'uint64' : ('uint64_t', 'uint64_t *', 'Q'),
}

def cintype(f):
	return field_types[f[0]][0]

def couttype(f):
	return field_types[f[0]][1]

def pack_fmt(fields):
	return ''.join(field_types[f[0]][2] for f in fields)

def op_pack_fmt(r):
	return 'II' + pack_fmt(r.fields)

def rec_pack_fmt(r):
	return 'I' + pack_fmt(r.fields)

#####################################################################
# Update log.h with #defines for types
#####################################################################
log_defines = (
	''.join(
		'#define\t%s\t%d\n' % (r.macro_name(), i) for i, r in enumerate(log_data.rectypes)) +
	''.join(
		'#define\t%s\t%d\n' % (r.macro_name(), i) for i, r in enumerate(log_data.optypes))
)

tfile = open(tmp_file, 'w')
skip = 0
for line in open('../src/include/log.h', 'r'):
	if skip:
		if 'Log record declarations: END' in line:
			tfile.write('/*\n' + line)
			skip = 0
	else:
		tfile.write(line)
	if 'Log record declarations: BEGIN' in line:
		skip = 1
		tfile.write(' */\n')
		tfile.write(log_defines)
tfile.close()
compare_srcfile(tmp_file, '../src/include/log.h')

#####################################################################
# Create log_auto.c with handlers for each record / operation type.
#####################################################################
f='../src/log/log_auto.c'
tfile = open(tmp_file, 'w')

tfile.write('/* DO NOT EDIT: automatically built by dist/log.py. */\n')

tfile.write('''
#include "wt_internal.h"

int
__wt_logrec_alloc(WT_SESSION_IMPL *session, WT_ITEM **logrecp)
{
	WT_ITEM *logrec;

	WT_RET(__wt_scr_alloc(session, LOG_ALIGN, &logrec));
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

# Emit code to read, write and print log records
for rectype in log_data.rectypes:
	pass

# Emit code to read, write and print log operations (within a log record)
for optype in log_data.optypes:
	if not optype.fields:
		continue

	tfile.write('''
int
__wt_logop_%(name)s_pack(
    WT_SESSION_IMPL *session, WT_ITEM *logrec%(arg_decls)s)
{
	const char *fmt = WT_UNCHECKED_STRING(%(fmt)s);
	size_t size;
	uint32_t optype, recsize;

	optype = %(macro)s;
	WT_RET(__wt_struct_size(session, &size, fmt,
	    optype, 0%(arg_names)s));

	size += __wt_vsize_uint(size) - 1;
	WT_RET(__wt_buf_grow(session, logrec, logrec->size + size));
	recsize = (uint32_t)size;
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    optype, recsize%(arg_names)s));

	logrec->size += size;
	return (0);
}
''' % {
	'name' : optype.name,
	'macro' : optype.macro_name(),
	'arg_decls' : ',\n    ' + ', '.join('%s%s%s' % (cintype(f), '' if cintype(f)[-1] == '*' else ' ', f[1]) for f in optype.fields),
	'arg_names' : ''.join(', %s' % f[1] for f in optype.fields),
	'fmt' : op_pack_fmt(optype)
})

	tfile.write('''
int
__wt_logop_%(name)s_unpack(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end%(arg_decls)s)
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
	'arg_decls' : ',\n    ' + ', '.join('%s%sp' % (couttype(f), f[1]) for f in optype.fields),
	'arg_names' : ''.join(', %sp' % f[1] for f in optype.fields),
	'fmt' : op_pack_fmt(optype)
})

tfile.write('''
#if 0
static WT_LOGREC_DESC __logrecs[] = {
''')

for rectype in log_data.rectypes:
	tfile.write('\t{ "%s", %s, },\n' % (rec_pack_fmt(rectype), rectype.prname()))

tfile.write('''};
#endif
''')

tfile.close()
compare_srcfile(tmp_file, f)
