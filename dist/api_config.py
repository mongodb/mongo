#!/usr/bin/env python

import os, re, sys, textwrap
import api_data
from dist import compare_srcfile

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
        'category': 'a set of related configuration options defined below',
        'string'  : 'a string'}[ctype]
    if cmin and cmax:
        desc += ' between ' + cmin + ' and ' + cmax
    elif cmin:
        desc += ' greater than or equal to ' + cmin
    elif cmax:
        desc += ' no more than ' + cmax
    if choices:
        if ctype == 'list':
            desc += ', with values chosen from the following options: '
        else:
            desc += ', chosen from the following options: '
        desc += ', '.join('\\c "' + c + '"' for c in choices)
    elif ctype == 'list':
        desc += ' of strings'
    return desc

def parseconfig(c, name_indent=''):
    ctype = gettype(c)
    desc = whitespace_re.sub(' ', c.desc.strip()) + '.'
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
        for subc in c.subconfig:
            output += parseconfig(subc, name_indent + ('&nbsp;' * 4))
        output += '@config{ ),,}\n'
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
            check = check + '\n\t    ' + cstr + ',\n\t    ' + sstr + '},'
        else:
            check = check + ' ' + cstr + ', ' + sstr + '},'
    else:
        check = '\n\t    '.join(w.wrap(check + ' ' + cstr + ', ' + sstr + '},'))
    return check

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
    if config_name not in api_data.methods:
        print >>sys.stderr, "Missing configuration for " + config_name
        tfile.write(line)
        continue

    skip = ('@configstart' in line)

    if not api_data.methods[config_name].config:
        tfile.write(prefix + '@configempty{' + config_name +
                ', see dist/api_data.py}\n')
        continue

    tfile.write(prefix + '@configstart{' + config_name +
            ', see dist/api_data.py}\n')

    w = textwrap.TextWrapper(width=80-len(prefix.expandtabs()),
            break_on_hyphens=False,
			replace_whitespace=False,
			fix_sentence_endings=True)
    lastname = None
    for c in sorted(api_data.methods[config_name].config):
        name = c.name
        if '.' in name:
            print >>sys.stderr, "Bad config key " + name

        # Deal with duplicates: with complex configurations (like
        # WT_SESSION::create), it's simpler to deal with duplicates here than
        # manually in api_data.py.
        if name == lastname:
            continue
        lastname = name
        if 'undoc' in c.flags:
            continue
        output = parseconfig(c)
        for l in w.wrap(output):
            tfile.write(prefix + l.replace('\n', '\n' + prefix) + '\n')

    tfile.write(prefix + '@configend\n')

tfile.close()
compare_srcfile(tmp_file, f)

#####################################################################
# Create config_def.c with defaults for each config string
#####################################################################
f='../src/config/config_def.c'
tfile = open(tmp_file, 'w')

tfile.write('''/* DO NOT EDIT: automatically built by dist/config.py. */

#include "wt_internal.h"
''')

# Make a TextWrapper that can wrap at commas.
w = textwrap.TextWrapper(width=68, break_on_hyphens=False)
w.wordsep_re = w.wordsep_simple_re = re.compile(r'(,)')

def checkstr(c):
    '''Generate the JSON string used by __wt_config_check to validate the
    config string'''
    checks = c.flags
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
        return '"' + ','.join(result) + '"'
    else:
        return 'NULL'

def get_default(c):
    t = gettype(c)
    if c.default == 'false':
        return '0'
    elif t == 'category':
        return '(%s)' % (','.join('%s=%s' % (subc.name, get_default(subc))
            for subc in sorted(c.subconfig)))
    elif (c.default or t == 'int') and c.default != 'true':
	return str(c.default).replace('"', '\\"')
    else:
        return ''

created_subconfigs=set()
def add_subconfig(c):
    if c.name in created_subconfigs:
        return
    created_subconfigs.add(c.name)
    tfile.write('''
static const WT_CONFIG_CHECK confchk_%(name)s_subconfigs[] = {
\t%(check)s
\t{ NULL, NULL, NULL, NULL }
};
''' % {
    'name' : c.name,
    'check' : '\n\t'.join('"\n\t    "'.join(w.wrap('{ "%s", "%s", %s, NULL },' %
        (subc.name, gettype(subc), checkstr(subc)))) for subc in sorted(c.subconfig)),
})

def getsubconfigstr(c):
    '''Return a string indicating if an item has sub configuration'''
    ctype = gettype(c)
    if ctype == 'category':
        add_subconfig(c)
        return 'confchk_' + c.name + '_subconfigs'
    else:
        return 'NULL'

# Write structures of arrays of allowable configuration options, including an
# empty string as a terminator for iteration.
for name in sorted(api_data.methods.keys()):
    ctype = api_data.methods[name].config
    if ctype:
        tfile.write('''
static const WT_CONFIG_CHECK confchk_%(name)s[] = {
\t%(check)s
\t{ NULL, NULL, NULL, NULL }
};
''' % {
    'name' : name.replace('.', '_'),
    'check' : '\n\t'.join(getconfcheck(c) for c in sorted(ctype)),
})

# Write the initialized list of configuration entry structures.
tfile.write('\n')
tfile.write('static const WT_CONFIG_ENTRY config_entries[] = {')

slot=-1
config_defines = ''
for name in sorted(api_data.methods.keys()):
    ctype = api_data.methods[name].config
    slot += 1
    name = name.replace('.', '_')

    # Build a list of #defines that reference specific slots in the list (the
    # #defines are used to avoid a list search where we know the correct slot).
    config_defines +=\
	'#define\tWT_CONFIG_ENTRY_' + name + '\t' * \
	    max(1, 6 - int ((len('WT_CONFIG_ENTRY_' + name)) / 8)) + \
	    "%2s" % str(slot) + '\n'

    # Write the method name, the uri and the value.
    tfile.write('''
\t{ "%(name)s",
\t  NULL,
%(config)s,''' % {
    'config' : '\n'.join('\t  "%s"' % line
        for line in w.wrap(','.join('%s=%s' % (c.name, get_default(c))
            for c in sorted(ctype))) or [""]),
    'name' : name
})

    # Write the checks reference, or NULL if no related checks structure.
    tfile.write('\n\t  ')
    name = name.replace('.', '_')
    if ctype:
        tfile.write('confchk_' + name)
    else:
        tfile.write('NULL')

    # Write the extended reference, always NULL initially, this slot is
    # used when the information is extended at run-time.
    tfile.write(',\n\t  NULL\n\t},')

tfile.write('\n};\n')

# Write the routine that connects the WT_CONNECTION_IMPL structure to the list
# of configuration entry structures.
tfile.write('''
void
__wt_conn_config_init(WT_CONNECTION_IMPL *conn)
{
	conn->config_entries = config_entries;
}
''')

tfile.close()
compare_srcfile(tmp_file, f)

# Update the config.h file with the #defines for the configuration entries.
tfile = open(tmp_file, 'w')
skip = 0
for line in open('../src/include/config.h', 'r'):
	if skip:
		if line.count('configuration section: END'):
			tfile.write('/*\n' + line)
			skip = 0
	else:
		tfile.write(line)
	if line.count('configuration section: BEGIN'):
		skip = 1
		tfile.write(' */\n')
		tfile.write(config_defines)
tfile.close()
compare_srcfile(tmp_file, '../src/include/config.h')
