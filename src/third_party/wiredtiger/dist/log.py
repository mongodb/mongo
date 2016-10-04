#!/usr/bin/env python

import os, re, sys, textwrap
from dist import compare_srcfile
import log_data

# Temporary file.
tmp_file = '__tmp'

# Map log record types to:
# (C type, pack type, printf format, printf arg(s), list of setup functions)
field_types = {
    'string' : ('const char *', 'S', '%s', 'arg', [ '' ]),
    'item' : ('WT_ITEM *', 'u', '%s', 'escaped',
        [ 'WT_ERR(__logrec_make_json_str(session, &escaped, &arg));',
          'WT_ERR(__logrec_make_hex_str(session, &escaped, &arg));']),
    'recno' : ('uint64_t', 'r', '%" PRIu64 "', 'arg', [ '' ]),
    'uint32' : ('uint32_t', 'I', '%" PRIu32 "', 'arg', [ '' ]),
    'uint64' : ('uint64_t', 'Q', '%" PRIu64 "', 'arg', [ '' ]),
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

def escape_decl(fields):
    return '\n\tchar *escaped;' if has_escape(fields) else ''

def has_escape(fields):
    for f in fields:
        for setup in field_types[f[0]][4]:
            if 'escaped' in setup:
                return True
    return False

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
    return ' ' + arg

def printf_setup(f, i, nl_indent):
    stmt = field_types[f[0]][4][i].replace('arg', f[1])
    return '' if stmt == '' else stmt + nl_indent

def n_setup(f):
    return len(field_types[f[0]][4])

# Create a printf line, with an optional setup function.
# ishex indicates that the the field name in the output is modified
# (to add "-hex"), and that the setup and printf are conditional
# in the generated code.
def printf_line(f, optype, i, ishex):
    ifbegin = ''
    ifend = ''
    nl_indent = '\n\t'
    name = f[1]
    postcomma = '' if i + 1 == len(optype.fields) else ',\\n'
    precomma = ''
    if ishex > 0:
        name += '-hex'
        ifend = nl_indent + '}'
        nl_indent += '\t'
        ifbegin = 'if (LF_ISSET(WT_TXN_PRINTLOG_HEX)) {' + nl_indent
        if postcomma == '':
            precomma = ',\\n'
    body = '%s%s(__wt_fprintf(session, WT_STDOUT(session),' % (
        printf_setup(f, ishex, nl_indent),
        'WT_ERR' if has_escape(optype.fields) else 'WT_RET') + \
        '%s    "%s        \\"%s\\": \\"%s\\"%s",%s));' % (
        nl_indent, precomma, name, printf_fmt(f), postcomma,
        printf_arg(f))
    return ifbegin + body + ifend

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
\tWT_ITEM *logrec;

\tWT_RET(
\t    __wt_scr_alloc(session, WT_ALIGN(size + 1, WT_LOG_ALIGN), &logrec));
\tWT_CLEAR(*(WT_LOG_RECORD *)logrec->data);
\tlogrec->size = offsetof(WT_LOG_RECORD, record);

\t*logrecp = logrec;
\treturn (0);
}

void
__wt_logrec_free(WT_SESSION_IMPL *session, WT_ITEM **logrecp)
{
\t__wt_scr_free(session, logrecp);
}

int
__wt_logrec_read(WT_SESSION_IMPL *session,
    const uint8_t **pp, const uint8_t *end, uint32_t *rectypep)
{
\tuint64_t rectype;

\tWT_UNUSED(session);
\tWT_RET(__wt_vunpack_uint(pp, WT_PTRDIFF(end, *pp), &rectype));
\t*rectypep = (uint32_t)rectype;
\treturn (0);
}

int
__wt_logop_read(WT_SESSION_IMPL *session,
    const uint8_t **pp, const uint8_t *end,
    uint32_t *optypep, uint32_t *opsizep)
{
\treturn (__wt_struct_unpack(
\t    session, *pp, WT_PTRDIFF(end, *pp), "II", optypep, opsizep));
}

static size_t
__logrec_json_unpack_str(char *dest, size_t destlen, const u_char *src,
    size_t srclen)
{
\tsize_t total;
\tsize_t n;

\ttotal = 0;
\twhile (srclen > 0) {
\t\tn = __wt_json_unpack_char(
\t\t    *src++, (u_char *)dest, destlen, false);
\t\tsrclen--;
\t\tif (n > destlen)
\t\t\tdestlen = 0;
\t\telse {
\t\t\tdestlen -= n;
\t\t\tdest += n;
\t\t}
\t\ttotal += n;
\t}
\tif (destlen > 0)
\t\t*dest = '\\0';
\treturn (total + 1);
}

static int
__logrec_make_json_str(WT_SESSION_IMPL *session, char **destp, WT_ITEM *item)
{
\tsize_t needed;

\tneeded = __logrec_json_unpack_str(NULL, 0, item->data, item->size);
\tWT_RET(__wt_realloc(session, NULL, needed, destp));
\t(void)__logrec_json_unpack_str(*destp, needed, item->data, item->size);
\treturn (0);
}

static int
__logrec_make_hex_str(WT_SESSION_IMPL *session, char **destp, WT_ITEM *item)
{
\tsize_t needed;

\tneeded = item->size * 2 + 1;
\tWT_RET(__wt_realloc(session, NULL, needed, destp));
\t__wt_fill_hex(item->data, item->size, (uint8_t *)*destp, needed, NULL);
\treturn (0);
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
\tconst char *fmt = WT_UNCHECKED_STRING(%(fmt)s);
\tsize_t size;
\tuint32_t optype, recsize;

\toptype = %(macro)s;
\tWT_RET(__wt_struct_size(session, &size, fmt,
\t    optype, 0%(arg_names)s));

\t__wt_struct_size_adjust(session, &size);
\tWT_RET(__wt_buf_extend(session, logrec, logrec->size + size));
\trecsize = (uint32_t)size;
\tWT_RET(__wt_struct_pack(session,
\t    (uint8_t *)logrec->data + logrec->size, size, fmt,
\t    optype, recsize%(arg_names)s));

\tlogrec->size += (uint32_t)size;
\treturn (0);
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
\tconst char *fmt = WT_UNCHECKED_STRING(%(fmt)s);
\tuint32_t optype, size;

\tWT_RET(__wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), fmt,
\t    &optype, &size%(arg_names)s));
\tWT_ASSERT(session, optype == %(macro)s);

\t*pp += size;
\treturn (0);
}
''' % {
    'name' : optype.name,
    'macro' : optype.macro_name(),
    'arg_decls' : ', '.join(
        '%s%sp' % (couttype(f), f[1]) for f in optype.fields),
    'arg_names' : ''.join(', %sp' % f[1] for f in optype.fields),
    'fmt' : op_pack_fmt(optype)
})

    last_field = optype.fields[-1]
    tfile.write('''
int
__wt_logop_%(name)s_print(WT_SESSION_IMPL *session,
    const uint8_t **pp, const uint8_t *end, uint32_t flags)
{
%(arg_ret)s\t%(arg_decls)s

\t%(arg_unused)s%(arg_init)sWT_RET(__wt_logop_%(name)s_unpack(
\t    session, pp, end%(arg_addrs)s));

\tWT_RET(__wt_fprintf(session, WT_STDOUT(session),
\t    " \\"optype\\": \\"%(name)s\\",\\n"));
\t%(print_args)s
%(arg_fini)s
}
''' % {
    'name' : optype.name,
    'arg_ret' : ('\tWT_DECL_RET;\n' if has_escape(optype.fields) else ''),
    'arg_decls' : ('\n\t'.join('%s%s%s;' %
        (clocaltype(f), '' if clocaltype(f)[-1] == '*' else ' ', f[1])
        for f in optype.fields)) + escape_decl(optype.fields),
    'arg_unused' : ('' if has_escape(optype.fields)
        else 'WT_UNUSED(flags);\n\t'),
    'arg_init' : ('escaped = NULL;\n\t' if has_escape(optype.fields) else ''),
    'arg_fini' : ('\nerr:\t__wt_free(session, escaped);\n\treturn (ret);'
    if has_escape(optype.fields) else '\treturn (0);'),
    'arg_addrs' : ''.join(', &%s' % f[1] for f in optype.fields),
    'print_args' : '\n\t'.join(printf_line(f, optype, i, s)
        for i,f in enumerate(optype.fields) for s in range(0, n_setup(f)))
})

# Emit the printlog entry point
tfile.write('''
int
__wt_txn_op_printlog(WT_SESSION_IMPL *session,
    const uint8_t **pp, const uint8_t *end, uint32_t flags)
{
\tuint32_t optype, opsize;

\t/* Peek at the size and the type. */
\tWT_RET(__wt_logop_read(session, pp, end, &optype, &opsize));
\tend = *pp + opsize;

\tswitch (optype) {''')

for optype in log_data.optypes:
    if not optype.fields:
        continue

    tfile.write('''
\tcase %(macro)s:
\t\tWT_RET(%(print_func)s(session, pp, end, flags));
\t\tbreak;
''' % {
    'macro' : optype.macro_name(),
    'print_func' : '__wt_logop_' + optype.name + '_print',
})

tfile.write('''
\tWT_ILLEGAL_VALUE(session);
\t}

\treturn (0);
}
''')

tfile.close()
compare_srcfile(tmp_file, f)
