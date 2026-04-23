#!/usr/bin/env python3

import os, log_data
from dist import compare_srcfile, format_srcfile

# Temporary file.
tmp_file = '__tmp_log' + str(os.getpid())

field_types = {}

class FieldType:
    def __init__(self, typename, ctype, packtype,
                 printf_fmt_templ, printf_arg_templ, setup, always_hex, byptr):
        self.typename = typename                  # Internal type id
        self.ctype = ctype                        # C type
        self.packtype = packtype                  # pack type
        self.printf_fmt_templ = printf_fmt_templ  # printf format or list of printf formats
        self.printf_arg_templ = printf_arg_templ  # printf arg
        self.setup = setup                        # list of setup functions
        self.always_hex = always_hex              # always include hex
        self.byptr = byptr                        # passed by pointer

    @staticmethod
    def Add(*args):
        field_types[args[0]] = FieldType(*args)

FieldType.Add('WT_LSN',
    'WT_LSN', 'II', '[%" PRIu32 ", %" PRIu32 "]',
    'arg.l.file, arg.l.offset', [ '' ], False, True),
FieldType.Add('string',
    'const char *', 'S', '\\"%s\\"', 'arg', [ '' ], False, False),
FieldType.Add('WT_ITEM',
    'WT_ITEM', 'u', '\\"%s\\"', '(char *)escaped->mem',
    [ 'WT_ERR(__logrec_make_json_str(session, &escaped, &arg));',
      'WT_ERR(__logrec_make_hex_str(session, &escaped, &arg));'], False, True),
FieldType.Add('recno',
    'uint64_t', 'r', '%" PRIu64 "', 'arg', [ '' ], False, False),
FieldType.Add('uint32_t',
    'uint32_t', 'I', '%" PRIu32 "', 'arg', [ '' ], False, False),
# The fileid may have the high bit set. Print in both decimal and hex.
FieldType.Add('uint32_id',
    'uint32_t', 'I', [ '%" PRIu32 "', '\\"0x%" PRIx32 "\\"' ], 'arg', [ '', '' ], True, False),
FieldType.Add('uint64_t',
    'uint64_t', 'Q', '%" PRIu64 "', 'arg', [ '' ], False, False),


class Field:
    def __init__(self, field_tuple, field_idx, fields_count):
        # Copy all attributes from FieldType object into this object.
        self.__dict__.update(field_types[field_tuple[0]].__dict__)
        self.fieldname = field_tuple[1]
        self.fieldidx = field_idx
        self.fieldcount = fields_count
        self.is_last_field = (field_idx + 1 >= fields_count)

        self.cintype = self.ctype + ('*' if self.byptr else '')
        self.cindecl = self.ctype + (' *' if self.byptr else ' ') + self.fieldname
        self.couttype = self.ctype + '*'
        self.coutdecl = self.ctype + ' *' + self.fieldname + 'p'
        self.clocaldef = self.ctype + ' ' + self.fieldname
        self.n_setup = len(self.setup)
        self.printf_arg = ' ' + self.printf_arg_templ.replace('arg', self.fieldname)

        # Override functions for this type.
        for func in ['pack_arg', 'unpack_arg',
                     'struct_size_body', 'struct_pack_body', 'struct_unpack_body']:
            if (func + '_' + self.typename) in dir(self):
                setattr(self, func, getattr(self, func + '_' + self.typename))

    def printf_fmt(self, ishex):
        fmt = self.printf_fmt_templ
        if type(fmt) is list:
            fmt = fmt[ishex]
        return fmt

    def pack_arg(self):
        return self.fieldname

    def unpack_arg(self):
        return self.fieldname + 'p'

    def printf_setup(self, i, nl_indent):
        stmt = self.setup[i].replace('arg', self.fieldname)
        return '' if stmt == '' else stmt + nl_indent

    # struct_size_body functions

    def struct_size_body(self):
        return '__wt_vsize_uint(%s)' % self.fieldname

    def struct_size_body_WT_LSN(self):
        return '__wt_vsize_uint(%(name)s->l.file) + __wt_vsize_uint(%(name)s->l.offset)' \
            % {'name' : self.fieldname}

    def struct_size_body_WT_ITEM(self):
        return \
            '__wt_vsize_uint(%(name)s->size) + %(name)s->size' % {'name' : self.fieldname} \
            if not self.is_last_field else \
            self.fieldname + '->size'

    def struct_size_body_string(self):
        return 'strlen(%s) + 1' % self.fieldname

    # struct_pack_body functions

    def struct_pack_body(self):
        return '    WT_RET(__pack_encode_uintAny(pp, end, %s));\n' % self.fieldname

    def struct_pack_body_WT_LSN(self):
        return  '''    WT_RET(__pack_encode_uintAny(pp, end, %(name)s->l.file));
    WT_RET(__pack_encode_uintAny(pp, end, %(name)s->l.offset));
''' % {'name' : self.fieldname}

    def struct_pack_body_WT_ITEM(self):
        fn = '__pack_encode_WT_ITEM' if not self.is_last_field else '__pack_encode_WT_ITEM_last'
        return '    WT_RET('+fn+'(pp, end, '+self.fieldname+'));\n'

    def struct_pack_body_string(self):
        return '    WT_RET(__pack_encode_string(pp, end, %s));\n' % self.fieldname

    # struct_unpack_body functions

    def struct_unpack_body(self):
        return '__pack_decode_uintAny(pp, end, %s, %sp);\n' % (self.cintype, self.fieldname)

    def struct_unpack_body_WT_LSN(self):
        return '''__pack_decode_uintAny(pp, end, uint32_t, &%(name)sp->l.file);
    __pack_decode_uintAny(pp, end, uint32_t, &%(name)sp->l.offset);
''' % {'name':self.fieldname}

    def struct_unpack_body_WT_ITEM(self):
        fn = '__pack_decode_WT_ITEM' if not self.is_last_field else '__pack_decode_WT_ITEM_last'
        return fn+'(pp, end, '+self.fieldname+'p);\n'

    def struct_unpack_body_string(self):
        return '__pack_decode_string(pp, end, %sp);\n' % self.fieldname


for op in log_data.optypes:
    op.fields = [ Field(f, i, len(op.fields)) for i, f in enumerate(op.fields) ]


def escape_decl(fields):
    return '\n    WT_DECL_ITEM(escaped);' if has_escape(fields) else ''

def has_escape(fields):
    for f in fields:
        for setup in f.setup:
            if 'escaped' in setup:
                return True
    return False

def pack_fmt(fields):
    return ''.join(f.packtype for f in fields)

def op_pack_fmt(r):
    return 'II' + pack_fmt(r.fields)

def rec_pack_fmt(r):
    return 'I' + pack_fmt(r.fields)


# Check for an operation that has a file id type. Redact any user data
# if the redact flag is set, but print operations for file id 0, known
# to be the metadata.
def check_redact(optype):
    for f in optype.fields:
        if f.typename == 'uint32_id':
            redact_str = '    if (!FLD_ISSET(args->flags, WT_TXN_PRINTLOG_UNREDACT) && '
            redact_str += '%s != WT_METAFILE_ID)\n' % (f.fieldname)
            redact_str += '        return(__wt_fprintf(session, args->fs, " REDACTED"));\n'
            return redact_str
    return ''


# Create a printf line, with an optional setup function.
# ishex indicates that the field name in the output is modified
# (to add "-hex"), and that the setup and printf are conditional
# in the generated code.
def printf_line(f, optype, i, ishex):
    ifbegin = ''
    ifend = ''
    nl_indent = '\n    '
    name = f.fieldname
    postcomma = '' if i + 1 == len(optype.fields) else ',\\n'
    precomma = ''
    if ishex > 0:
        name += '-hex'
        if not f.always_hex:
            ifend = nl_indent + '}'
            nl_indent += '    '
            ifbegin = \
                'if (FLD_ISSET(args->flags, WT_TXN_PRINTLOG_HEX)) {' + nl_indent
            if postcomma == '':
                precomma = ',\\n'
    body = '%s%s(__wt_fprintf(session, args->fs,' % (
        f.printf_setup(ishex, nl_indent),
        'WT_ERR' if has_escape(optype.fields) else 'WT_RET') + \
        '%s    "%s        \\"%s\\": %s%s",%s));' % (
        nl_indent, precomma, name, f.printf_fmt(ishex), postcomma,
        f.printf_arg)
    return ifbegin + body + ifend

#####################################################################
# Create log_auto.c with handlers for each record / operation type.
#####################################################################

def run():
    f='../src/log/log_auto.c'
    tfile = open(tmp_file, 'w')

    tfile.write('''/* DO NOT EDIT: automatically built by dist/log.py. */

#include "wt_internal.h"
#include "log_private.h"

#define WT_SIZE_CHECK_PACK_PTR(p, end)     WT_RET_TEST(!(p) || !(end) || (p) >= (end), ENOMEM)
#define WT_SIZE_CHECK_UNPACK_PTR(p, end)   WT_RET_TEST(!(p) || !(end) || (p) >= (end), EINVAL)
#define WT_SIZE_CHECK_UNPACK_PTR0(p, end)  WT_RET_TEST(!(p) || !(end) || (p) >  (end), EINVAL)

/*
 * Defining PACKING_COMPATIBILITY_MODE makes __wt_logop_*_unpack functions behave in a more
 * compatible way with older versions of WiredTiger and wiredtiger_struct_unpack(...fmt...)
 * function. This only alters the behavior for corrupted binary data, returning some value rather
 * than failing with EINVAL.
 */

#ifndef PACKING_COMPATIBILITY_MODE
#define WT_CHECK_OPTYPE(session, opvar, op) \
    if (opvar != op)                        \
        WT_RET_MSG(session, EINVAL, "unpacking " #op ": optype mismatch");
#else
#define WT_CHECK_OPTYPE(session, opvar, op)
#endif

/*
 * __pack_encode_uintAny --
 *     Pack an unsigned integer.
 */
static WT_INLINE int
__pack_encode_uintAny(uint8_t **pp, uint8_t *end, uint64_t item)
{
    /* Check that there is at least one byte available:
     * the low-level routines treat zero length as unchecked. */
    WT_SIZE_CHECK_PACK_PTR(*pp, end);
    return (__wt_vpack_uint(pp, WT_PTRDIFF(end, *pp), item));
}

/*
 * __pack_encode_WT_ITEM --
 *     Pack a WT_ITEM structure - size and WT_ITEM.
 */
static WT_INLINE int
__pack_encode_WT_ITEM(uint8_t **pp, uint8_t *end, WT_ITEM *item)
{
    WT_RET(__wt_vpack_uint(pp, WT_PTRDIFF(end, *pp), item->size));
    if (item->size != 0) {
        WT_SIZE_CHECK_PACK(item->size, WT_PTRDIFF(end, *pp));
        memcpy(*pp, item->data, item->size);
        *pp += item->size;
    }
    return (0);
}

/*
 * __pack_encode_WT_ITEM_last --
 *     Pack a WT_ITEM structure without its size.
 */
static WT_INLINE int
__pack_encode_WT_ITEM_last(uint8_t **pp, uint8_t *end, WT_ITEM *item)
{
    if (item->size != 0) {
        WT_SIZE_CHECK_PACK(item->size, WT_PTRDIFF(end, *pp));
        memcpy(*pp, item->data, item->size);
        *pp += item->size;
    }
    return (0);
}

/*
 * __pack_encode_string --
 *     Pack a string.
 */
static WT_INLINE int
__pack_encode_string(uint8_t **pp, uint8_t *end, const char *item)
{
    size_t s, sz;

    sz = WT_PTRDIFF(end, *pp);
    s = __wt_strnlen(item, sz - 1);
    WT_SIZE_CHECK_PACK(s + 1, sz);
    memcpy(*pp, item, s);
    *pp += s;
    **pp = '\\0';
    *pp += 1;
    return (0);
}

#define __pack_decode_uintAny(pp, end, TYPE, pval)                                             \
    do {                                                                                       \
        uint64_t v; /* Check that there is at least one byte available: the low-level routines \
                       treat zero length as unchecked. */                                      \
        WT_SIZE_CHECK_UNPACK_PTR(*pp, end);                                                    \
        WT_RET(__wt_vunpack_uint(pp, WT_PTRDIFF(end, *pp), &v));                               \
        *(pval) = (TYPE)v;                                                                     \
    } while (0)

#define __pack_decode_WT_ITEM(pp, end, val)                    \
    do {                                                       \
        __pack_decode_uintAny(pp, end, size_t, &val->size);    \
        WT_SIZE_CHECK_UNPACK(val->size, WT_PTRDIFF(end, *pp)); \
        val->data = *pp;                                       \
        *pp += val->size;                                      \
    } while (0)

#define __pack_decode_WT_ITEM_last(pp, end, val) \
    do {                                         \
        WT_SIZE_CHECK_UNPACK_PTR0(*pp, end);     \
        val->size = WT_PTRDIFF(end, *pp);        \
        val->data = *pp;                         \
        *pp += val->size;                        \
    } while (0)

#define __pack_decode_string(pp, end, val)             \
    do {                                               \
        size_t s;                                      \
        *val = (const char *)*pp;                      \
        s = strlen((const char *)*pp) + 1;             \
        WT_SIZE_CHECK_UNPACK(s, WT_PTRDIFF(end, *pp)); \
        *pp += s;                                      \
    } while (0)

/*
 * __wt_logrec_alloc --
 *     Allocate a new WT_ITEM structure.
 */
int
__wt_logrec_alloc(WT_SESSION_IMPL *session, size_t size, WT_ITEM **logrecp)
{
    WT_ITEM *logrec;

    WT_RET(__wt_scr_alloc(session, WT_ALIGN(size + 1, WTI_LOG_ALIGN), &logrec));
    WT_CLEAR(*(WT_LOG_RECORD *)logrec->data);
    logrec->size = offsetof(WT_LOG_RECORD, record);

    *logrecp = logrec;
    return (0);
}

/*
 * __wt_logrec_free --
 *     Free the given WT_ITEM structure.
 */
void
__wt_logrec_free(WT_SESSION_IMPL *session, WT_ITEM **logrecp)
{
    __wt_scr_free(session, logrecp);
}

/*
 * __wt_logrec_read --
 *     Read the record type.
 */
int
__wt_logrec_read(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, uint32_t *rectypep)
{
    WT_UNUSED(session);
    __pack_decode_uintAny(pp, end, uint32_t, rectypep);
    return (0);
}

/*
 * __wt_logop_read --
 *     Peek at the operation type.
 */
int
__wt_logop_read(WT_SESSION_IMPL *session, const uint8_t **pp_peek, const uint8_t *end,
  uint32_t *optypep, uint32_t *opsizep)
{
    const uint8_t *p, **pp;
    WT_UNUSED(session);

    p = *pp_peek;
    pp = &p;
    __pack_decode_uintAny(pp, end, uint32_t, optypep);
    __pack_decode_uintAny(pp, end, uint32_t, opsizep);
    return (0);
}

/*
 * __wt_logop_unpack --
 *     Read the operation type.
 */
int
__wt_logop_unpack(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  uint32_t *optypep, uint32_t *opsizep)
{
    WT_UNUSED(session);
    __pack_decode_uintAny(pp, end, uint32_t, optypep);
    __pack_decode_uintAny(pp, end, uint32_t, opsizep);
    return (0);
}

/*
 * __wt_logop_write --
 *     Write the operation type.
 */
int
__wt_logop_write(
  WT_SESSION_IMPL *session, uint8_t **pp, uint8_t *end, uint32_t optype, uint32_t opsize)
{
    WT_UNUSED(session);
    WT_RET(__pack_encode_uintAny(pp, end, optype));
    WT_RET(__pack_encode_uintAny(pp, end, opsize));
    return (0);
}

/*
 * __logrec_make_json_str --
 *     Unpack a string into JSON escaped format.
 */
static int
__logrec_make_json_str(WT_SESSION_IMPL *session, WT_ITEM **escapedp, WT_ITEM *item)
{
    size_t needed;

    needed = (item->size * WT_MAX_JSON_ENCODE) + 1;

    if (*escapedp == NULL)
        WT_RET(__wt_scr_alloc(session, needed, escapedp));
    else
        WT_RET(__wt_buf_grow(session, *escapedp, needed));
    WT_IGNORE_RET(
      __wt_json_unpack_str((*escapedp)->mem, (*escapedp)->memsize, item->data, item->size));
    return (0);
}

/*
 * __logrec_make_hex_str --
 *     Convert data to a hexadecimal representation.
 */
static int
__logrec_make_hex_str(WT_SESSION_IMPL *session, WT_ITEM **escapedp, WT_ITEM *item)
{
    size_t needed;

    needed = (item->size * 2) + 1;

    if (*escapedp == NULL)
        WT_RET(__wt_scr_alloc(session, needed, escapedp));
    else
        WT_RET(__wt_buf_grow(session, *escapedp, needed));
    __wt_fill_hex(item->data, item->size, (*escapedp)->mem, (*escapedp)->memsize, NULL);
    return (0);
}

''')

    # Emit code to read, write and print log operations (within a log record)
    for optype in log_data.optypes:
        tfile.write('''
/*
 * __wt_struct_size_%(name)s --
 *     Calculate size of %(name)s struct.
 */
static WT_INLINE size_t
__wt_struct_size_%(name)s(%(arg_decls_in_or_void)s)
{
    return (%(size_body)s);
}


/*
 * __wt_struct_pack_%(name)s --
 *     Pack the %(name)s struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_pack_%(name)s(uint8_t **pp, uint8_t *end%(comma)s
    %(arg_decls_in)s)
{
    %(pack_body)s
    return (0);
}

/*
 * __wt_struct_unpack_%(name)s --
 *     Unpack the %(name)s struct.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
static WT_INLINE int
__wt_struct_unpack_%(name)s(const uint8_t **pp, const uint8_t *end%(comma)s
    %(arg_decls_out)s)
{
    %(unpack_body)s
    return (0);
}


/*
 * __wt_logop_%(name)s_pack --
 *     Pack the log operation %(name)s.
 */
WT_GCC_FUNC_DECL_ATTRIBUTE((warn_unused_result))
int
__wt_logop_%(name)s_pack(
    WT_SESSION_IMPL *session, WT_ITEM *logrec%(comma)s
    %(arg_decls_in)s)
{
    size_t size;
    uint8_t *buf, *end;

    size = __wt_struct_size_%(name)s(%(pack_args)s);
    size += __wt_vsize_uint(%(macro)s) + __wt_vsize_uint(0);
    __wt_struct_size_adjust(session, &size);
    WT_RET(__wt_buf_extend(session, logrec, logrec->size + size));

    buf = (uint8_t *)logrec->data + logrec->size;
    end = buf + size;
    WT_RET(__wt_logop_write(session, &buf, end, %(macro)s, (uint32_t)size));
    WT_RET(__wt_struct_pack_%(name)s(&buf, end%(comma)s%(pack_args)s));

    logrec->size += (uint32_t)size;
    return (0);
}

/*
 * __wt_logop_%(name)s_unpack --
 *     Unpack the log operation %(name)s.
 */
int
__wt_logop_%(name)s_unpack(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end%(comma)s
    %(arg_decls_out)s)
{
    WT_DECL_RET;
    uint32_t optype, size;

#if !defined(NO_STRICT_PACKING_CHECK) || defined(PACKING_COMPATIBILITY_MODE)
    const uint8_t *pp_orig;
    pp_orig = *pp;
#endif

    if ((ret = __wt_logop_unpack(session, pp, end, &optype, &size)) != 0 ||
            (ret = __wt_struct_unpack_%(name)s(pp, end%(comma)s%(unpack_args)s)) != 0)
        WT_RET_MSG(session, ret, "logop_%(name)s: unpack failure");

    WT_CHECK_OPTYPE(session, optype, %(macro)s);

#if !defined(NO_STRICT_PACKING_CHECK)
    if (WT_PTRDIFF(*pp, pp_orig) != size) {
        WT_RET_MSG(session, EINVAL, "logop_%(name)s: size mismatch: expected %%u, got %%" PRIuPTR,
            size, WT_PTRDIFF(*pp, pp_orig));
    }
#endif
#if defined(PACKING_COMPATIBILITY_MODE)
    *pp = pp_orig + size;
#endif

    return (0);
}
''' % {
            'name' : optype.name,
            'macro' : optype.macro_name,
            'comma' : ',' if optype.fields else '',
            'arg_decls_in' : ', '.join(f.cindecl for f in optype.fields),
            'arg_decls_in_or_void' : ', '.join(
                f.cindecl
                for f in optype.fields)
                if optype.fields else 'void',
            'arg_decls_out' : ', '.join(f.coutdecl for f in optype.fields),
            'size_body' : ' + '.join(
                f.struct_size_body()
                for f in optype.fields)
                if optype.fields else '0',
            'pack_args' : ', '.join(f.pack_arg() for f in optype.fields),
            'pack_body' : ''.join(
                f.struct_pack_body()
                for f in optype.fields)
                if optype.fields else '    WT_UNUSED(pp);\n    WT_UNUSED(end);',
            'unpack_args' : ', '.join(f.unpack_arg() for f in optype.fields),
            'unpack_body' : ''.join(
                f.struct_unpack_body()
                for f in optype.fields)
                if optype.fields else '    WT_UNUSED(pp);\n    WT_UNUSED(end);',
            'fmt' : op_pack_fmt(optype),
        })

        tfile.write('''
/*
 * __wt_logop_%(name)s_print --
 *    Print the log operation %(name)s.
 */
int
__wt_logop_%(name)s_print(WT_SESSION_IMPL *session,
    const uint8_t **pp, const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args)
{%(arg_ret)s%(local_decls)s

    WT_RET(__wt_logop_%(name)s_unpack(session, pp, end%(arg_addrs)s));

    %(redact)s
    WT_RET(__wt_fprintf(session, args->fs,
        " \\"optype\\": \\"%(name)s\\"%(comma)s\\n"));
%(print_args)s
%(arg_fini)s
}
''' % {
            'name' : optype.name,
            'comma' : ',' if len(optype.fields) > 0 else '',
            'arg_ret' : ('\n    WT_DECL_RET;' if has_escape(optype.fields) else ''),
            'local_decls' : (('\n' + '\n'.join("    " + f.clocaldef + ";"
                for f in optype.fields)) + escape_decl(optype.fields)
                if optype.fields else ''),
            'arg_fini' : ('\nerr:    __wt_scr_free(session, &escaped);\n    return (ret);'
            if has_escape(optype.fields) else '    return (0);'),
            'arg_addrs' : ''.join(', &%s' % f.fieldname for f in optype.fields),
            'redact' : check_redact(optype),
            'print_args' : ('    ' + '\n    '.join(printf_line(f, optype, i, s)
                for i,f in enumerate(optype.fields) for s in range(0, f.n_setup))
                if optype.fields else ''),
        })

    # Emit the printlog entry point
    tfile.write('''
/*
 * __wt_txn_op_printlog --
 *     Print operation from a log cookie.
 */
int
__wt_txn_op_printlog(WT_SESSION_IMPL *session,
    const uint8_t **pp, const uint8_t *end, WT_TXN_PRINTLOG_ARGS *args)
{
    uint32_t optype, opsize;

    /* Peek at the size and the type. */
    WT_RET(__wt_logop_read(session, pp, end, &optype, &opsize));
    end = *pp + opsize;

    switch (optype) {''')

    for optype in log_data.optypes:
        tfile.write('''
    case %(macro)s:
        WT_RET(%(print_func)s(session, pp, end, args));
        break;
''' % {
        'macro' : optype.macro_name,
        'print_func' : '__wt_logop_' + optype.name + '_print',
    })

    tfile.write('''
    default:\n        return (__wt_illegal_value(session, optype));
    }

    return (0);
}
''')

    tfile.close()
    format_srcfile(tmp_file)
    compare_srcfile(tmp_file, f)


if __name__ == "__main__":
    run()
