#!/usr/bin/env python

from __future__ import print_function
import os, re, sys, textwrap
from dist import compare_srcfile, format_srcfile

test_config = False

# This file serves two purposes, it can generate configuration for the main wiredtiger library and,
# it can generate configuration for the c and cpp suite tests. To avoid duplication we import the
# differing apis here and then treat them as the same for the remainder of the script. However we
# do have different logic depending on whether we intend to generate the test api or not, which is
# managed with a boolean flag.
if len(sys.argv) == 1 or sys.argv[1] != "-t":
    import api_data as api_data_def
else:
    test_config = True
    import test_data as api_data_def

# Temporary file.
tmp_file = '__tmp'

#####################################################################
# Update wiredtiger.in with doxygen comments
#####################################################################
f='../src/include/wiredtiger.in'
tfile = open(tmp_file, 'w')

whitespace_re = re.compile(r'\s+')
cbegin_re = re.compile(r'(\s*\*\s*)@config(?:empty|start)\{(.*?),.*\}')

def gettype(c):
    '''Derive the type of a config item'''
    checks = c.flags
    ctype = checks.get('type', None)
    if not ctype and ('min' in checks or 'max' in checks):
        ctype = 'int'
    return ctype or 'string'

def typedesc(c):
    '''Descripe what type of value is expected for the given config item'''
    checks = c.flags
    cmin = str(checks.get('min', ''))
    cmax = str(checks.get('max', ''))
    choices = checks.get('choices', [])
    ctype = gettype(c)
    desc = {
        'boolean' : 'a boolean flag',
        'format'  : 'a format string',
        'int'     : 'an integer',
        'list'    : 'a list',
        'category': 'a set of related configuration options defined as follows',
        'string'  : 'a string'}[ctype]
    if cmin and cmax:
        desc += ' between \c ' + cmin + ' and \c ' + cmax
    elif cmin:
        desc += ' greater than or equal to \c ' + cmin
    elif cmax:
        desc += ' no more than \c ' + cmax
    if choices:
        if ctype == 'list':
            desc += ', with values chosen from the following options: '
        else:
            desc += ', chosen from the following options: '
        desc += ', '.join('\\c "' + c + '"' for c in choices)
    elif ctype == 'list':
        desc += ' of strings'
    return desc

def parseconfig(c, method_name, name_indent=''):
    c.method_name = method_name
    ctype = gettype(c)
    desc = whitespace_re.sub(' ', c.desc.strip())
    desc = desc.strip('.') + '.'
    desc = desc.replace(',', '\\,')
    default = '\\c ' + str(c.default) if c.default or ctype == 'int' \
            else 'empty'
    name = name_indent + c.name

    tdesc = typedesc(c)
    if ctype != 'category':
        tdesc += '; default ' + default
    else:
        name += ' = ('
    tdesc += '.'
    tdesc = tdesc.replace(',', '\\,')
    output = '@config{' + ', '.join((name, desc, tdesc)) + '}\n'
    if ctype == 'category':
        for subc in sorted(c.subconfig):
            if 'undoc' in subc.flags:
                continue
            output += parseconfig(subc, method_name, \
                                name_indent + ('&nbsp;' * 4))
        output += '@config{' + name_indent + ' ),,}\n'
    return output

def getconfcheck(c):
    check = '{ "' + c.name + '", "' + gettype(c) + '",'
    cstr = checkstr(c)
    sstr = getsubconfigstr(c)
    if cstr != 'NULL':
        cstr = '"\n\t    "'.join(w.wrap(cstr))
        # Manually re-wrap when there is a check string to avoid ugliness
        # between string and non-string wrapping
        if len(check + ' ' + cstr + ',\n\t    ' + sstr + '},') >= 68:
            check = check + '\n\t    ' + cstr + ',\n\t    ' + sstr + ' },'
        else:
            check = check + ' ' + cstr + ', ' + sstr + ' },'
    else:
        check = '\n\t    '.join(
            w.wrap(check + ' ' + cstr + ', ' + sstr + ' },'))
    return check

if not test_config:
    skip = False
    for line in open(f, 'r'):
        if skip:
            if '@configend' in line:
                skip = False
            continue

        m = cbegin_re.match(line)
        if not m:
            tfile.write(line)
            continue

        prefix, config_name = m.groups()
        if config_name not in api_data_def.methods:
            print("Missing configuration for " + config_name, file=sys.stderr)
            tfile.write(line)
            continue

        skip = ('@configstart' in line)

        if not api_data_def.methods[config_name].config:
            tfile.write(prefix + '@configempty{' + config_name +
                    ', see dist/api_data.py}\n')
            continue

        tfile.write(prefix + '@configstart{' + config_name +
                ', see dist/api_data.py}\n')

        w = textwrap.TextWrapper(width=100-len(prefix.expandtabs()),
                break_on_hyphens=False,
                break_long_words=False,
                replace_whitespace=False,
                fix_sentence_endings=True)
        # Separate at spaces, and after a set of non-breaking space indicators.
        w.wordsep_re = w.wordsep_simple_re = \
            re.compile(r'(\s+|(?<=&nbsp;)[\w_,.;:]+)')
        for c in api_data_def.methods[config_name].config:
            if 'undoc' in c.flags:
                continue
            output = parseconfig(c, config_name)
            for l in w.wrap(output):
                tfile.write(prefix + l.replace('\n', '\n' + prefix) + '\n')

        tfile.write(prefix + '@configend\n')

    tfile.close()
    compare_srcfile(tmp_file, f)

#####################################################################
# Create config_def.c with defaults for each config string
#####################################################################
f='../src/config/config_def.c'
if test_config:
    f = '../src/config/test_config.c'
tfile = open(tmp_file, 'w')

tfile.write('''/* DO NOT EDIT: automatically built by dist/api_config.py. */

#include "wt_internal.h"
''')

# Make a TextWrapper that wraps at commas.
w = textwrap.TextWrapper(width=64, break_on_hyphens=False,
                         break_long_words=False)
w.wordsep_re = w.wordsep_simple_re = re.compile(r'(,)')

# TextWrapper that wraps at whitespace.
ws = textwrap.TextWrapper(width=64, break_on_hyphens=False,
                          break_long_words=False)

def checkstr(c):
    '''Generate the function reference and JSON string used by __wt_config_check
       to validate the config string'''
    checks = c.flags
    cfunc = str(checks.get('func', ''))
    if not cfunc:
        cfunc = 'NULL';
    cmin = str(checks.get('min', ''))
    cmax = str(checks.get('max', ''))
    choices = checks.get('choices', [])
    result = []
    if cmin:
        result.append('min=' + cmin)
    if cmax:
        result.append('max=' + cmax)
    if choices:
        result.append('choices=' + '[' +
            ','.join('\\"' + s + '\\"' for s in choices) + ']')
    if result:
        return cfunc + ', "' + ','.join(result) + '"'
    else:
        return cfunc + ', NULL'

def get_default(c):
    t = gettype(c)
    if c.default == 'false':
        return 'false'
    elif c.default == 'true':
        return 'true'
    elif t == 'string' and c.default == 'none' and \
        not c.flags.get('choices', []):
        return ''
    elif t == 'category':
        return '(%s)' % (','.join('%s=%s' % (subc.name, get_default(subc))
            for subc in sorted(c.subconfig)))
    elif c.default or t == 'int':
        return str(c.default).replace('"', '\\"')
    else:
        return ''

created_subconfigs=set()
def add_subconfig(c, cname):
    if cname in created_subconfigs:
        return
    created_subconfigs.add(cname)
    tfile.write('''
%(name)s[] = {
\t%(check)s
\t{ NULL, NULL, NULL, NULL, NULL, 0 }
};
''' % {
    'name' : '\n    '.join(ws.wrap(\
        'static const WT_CONFIG_CHECK confchk_' + cname + '_subconfigs')),
    'check' : '\n\t'.join(getconfcheck(subc) for subc in sorted(c.subconfig)),
})

def getcname(c):
    '''Return the C name of a sub configuration'''
    prefix = c.method_name.replace('.', '_') + '_' \
             if hasattr(c, 'method_name') else ''
    return prefix + c.name

def getsubconfigstr(c):
    '''Return a string indicating if an item has sub configuration'''
    ctype = gettype(c)
    if ctype == 'category':
        cname = getcname(c)
        add_subconfig(c, cname)
        return 'confchk_' + cname + '_subconfigs, ' + str(len(c.subconfig))
    else:
        return 'NULL, 0'

# Write structures of arrays of allowable configuration options, including a
# NULL as a terminator for iteration.
for name in sorted(api_data_def.methods.keys()):
    config = api_data_def.methods[name].config
    if config:
        tfile.write('''
static const WT_CONFIG_CHECK confchk_%(name)s[] = {
\t%(check)s
\t{ NULL, NULL, NULL, NULL, NULL, 0 }
};
''' % {
    'name' : name.replace('.', '_'),
    'check' : '\n\t'.join(getconfcheck(c) for c in config),
})

# Write the initialized list of configuration entry structures.
tfile.write('\n')
tfile.write('static const WT_CONFIG_ENTRY config_entries[] = {')

slot=-1
config_defines = ''
for name in sorted(api_data_def.methods.keys()):
    config = api_data_def.methods[name].config
    slot += 1

    # Build a list of #defines that reference specific slots in the list (the
    # #defines are used to avoid a list search where we know the correct slot).
    config_defines +=\
        '#define\tWT_CONFIG_ENTRY_' + name.replace('.', '_') + '\t' * \
            max(1, 6 - (len('WT_CONFIG_ENTRY_' + name) // 8)) + \
            "%2s" % str(slot) + '\n'

    # Write the method name and base.
    tfile.write('''
\t{ "%(name)s",
%(config)s,''' % {
    'config' : '\n'.join('\t  "%s"' % line
        for line in w.wrap(','.join('%s=%s' % (c.name, get_default(c))
            for c in config)) or [""]),
    'name' : name
})

    # Write the checks reference, or NULL if no related checks structure.
    tfile.write('\n\t  ')
    if config:
        tfile.write(
            'confchk_' + name.replace('.', '_') + ', ' + str(len(config)))
    else:
        tfile.write('NULL, 0')

    tfile.write('\n\t},')

# Write a NULL as a terminator for iteration.
tfile.write('\n\t{ NULL, NULL, NULL, 0 }')
tfile.write('\n};\n')

# Write the routine that connects the WT_CONNECTION_IMPL structure to the list
# of configuration entry structures.
if not test_config:
    tfile.write('''
    int
    __wt_conn_config_init(WT_SESSION_IMPL *session)
    {
    \tWT_CONNECTION_IMPL *conn;
    \tconst WT_CONFIG_ENTRY *ep, **epp;

    \tconn = S2C(session);

    \t/* Build a list of pointers to the configuration information. */
    \tWT_RET(__wt_calloc_def(session, WT_ELEMENTS(config_entries), &epp));
    \tconn->config_entries = epp;

    \t/* Fill in the list to reference the default information. */
    \tfor (ep = config_entries;;) {
    \t\t*epp++ = ep++;
    \t\tif (ep->method == NULL)
    \t\t\tbreak;
    \t}
    \treturn (0);
    }

    void
    __wt_conn_config_discard(WT_SESSION_IMPL *session)
    {
    \tWT_CONNECTION_IMPL *conn;

    \tconn = S2C(session);

    \t__wt_free(session, conn->config_entries);
    }

    /*
    * __wt_conn_config_match --
    *      Return the static configuration entry for a method.
    */
    const WT_CONFIG_ENTRY *
    __wt_conn_config_match(const char *method)
    {
    \tconst WT_CONFIG_ENTRY *ep;

    \tfor (ep = config_entries; ep->method != NULL; ++ep)
    \t\tif (strcmp(method, ep->method) == 0)
    \t\t\treturn (ep);
    \treturn (NULL);
    }
    ''')
else:
    tfile.write(
    '''
    /*
        * __wt_test_config_match --
        *     Return the static configuration entry for a test.
        */
    const WT_CONFIG_ENTRY *
    __wt_test_config_match(const char *test_name)
    {
        const WT_CONFIG_ENTRY *ep;

        for (ep = config_entries; ep->method != NULL; ++ep)
            if (strcmp(test_name, ep->method) == 0)
                return (ep);
        return (NULL);
    }
    '''
    )

tfile.close()
format_srcfile(tmp_file)
compare_srcfile(tmp_file, f)

# Update the config.h file with the #defines for the configuration entries.
if not test_config:
    tfile = open(tmp_file, 'w')
    skip = 0
    config_file = '../src/include/config.h'
    for line in open(config_file, 'r'):
        if skip:
            if 'configuration section: END' in line:
                tfile.write('/*\n' + line)
                skip = 0
        else:
            tfile.write(line)
        if 'configuration section: BEGIN' in line:
            skip = 1
            tfile.write(' */\n')
            tfile.write(config_defines)
    tfile.close()
    format_srcfile(tmp_file)
    compare_srcfile(tmp_file, config_file)
