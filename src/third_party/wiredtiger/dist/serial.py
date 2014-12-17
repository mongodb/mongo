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
        SerialArg('WT_INSERT_HEAD *', 'ins_head'),
        SerialArg('WT_INSERT ***', 'ins_stack'),
        SerialArg('WT_INSERT *', 'new_ins', 1),
        SerialArg('uint64_t *', 'recnop'),
        SerialArg('u_int', 'skipdepth'),
    ]),

Serial('insert', [
        SerialArg('WT_INSERT_HEAD *', 'ins_head'),
        SerialArg('WT_INSERT ***', 'ins_stack'),
        SerialArg('WT_INSERT *', 'new_ins', 1),
        SerialArg('u_int', 'skipdepth'),
    ]),

Serial('update', [
        SerialArg('WT_UPDATE **', 'srch_upd'),
        SerialArg('WT_UPDATE *', 'upd', 1),
    ]),
]

# decl --
#    Return a declaration for the variable.
def decl(l):
    o = l.typestr
    if o[-1] != '*':
        o += ' '
    return o + l.name

# decl_p --
#    Return a declaration for a reference to the variable, which requires
# another level of indirection.
def decl_p(l):
    o = l.typestr
    if o[-1] != '*':
        o += ' '
    return o + '*' + l.name + 'p'

# output --
#    Create serialized function calls.
def output(entry, f):
    # Function declaration.
    f.write('static inline int\n__wt_' + entry.name + '_serial(\n')
    o = 'WT_SESSION_IMPL *session, WT_PAGE *page'
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

    # Check the page write generation hasn't wrapped.
    f.write('''
\t/*
\t * Check to see if the page's write generation is about to wrap (wildly
\t * unlikely as it implies 4B updates between clean page reconciliations,
\t * but technically possible), and fail the update.
\t *
\t * The check is outside of the serialization mutex because the page's
\t * write generation is going to be a hot cache line, so technically it's
\t * possible for the page's write generation to wrap between the test and
\t * our subsequent modification of it.  However, the test is (4B-1M), and
\t * there cannot be a million threads that have done the test but not yet
\t * completed their modification.
\t */
\t WT_RET(__page_write_gen_wrapped_check(page));
''')

    # Call the worker function.
    if entry.name != "update":
        f.write('''
\t/* Acquire the page's spinlock, call the worker function. */
\tWT_PAGE_LOCK(session, page);''')

    f.write('''
\tret = __''' + entry.name + '''_serial_func(
''')
    o = 'session'
    if entry.name == "update":
        o += ', page'
    for l in entry.args:
        o += ', ' + l.name
    o += ');'
    f.write('\n'.join('\t    ' + l for l in textwrap.wrap(o, 70)))

    if entry.name != "update":
        f.write('''
\tWT_PAGE_UNLOCK(session, page);''')

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
