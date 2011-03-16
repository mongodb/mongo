#!/usr/bin/env python

import os, re, sys, textwrap
import config_data
from dist import compare_srcfile

# Temporary file.
tmp_file = '__tmp'

#####################################################################
# Update wiredtiger.in with doxygen comments
#####################################################################
f='../src/include/wiredtiger.in'
tfile = open(tmp_file, 'w')

cbegin_re = re.compile(r'(\s*\*\s*)@configstart\{(.*?),.*\}')

skip = False
for line in open(f, 'r'):
	if skip:
		if '@configend' in line:
			tfile.write(line)
			skip = False
		continue
	tfile.write(line)

	m = cbegin_re.match(line)
	if not m:
		continue

	prefix, config_name = m.groups()
	if config_name not in config_data.config_types:
		print >>sys.stderr, "Missing configuration for " + config_name
		continue

	width = 80 - len(prefix.expandtabs())
	for c in config_data.config_types[config_name]:
		desc = textwrap.dedent(c.desc).replace(',', '\\,')
		name = c.name
		if '.' in name:
			name = '%s.\\<%s\\>' % tuple(name.split('.'))
		output = '@config{' + name + ',' + desc + '}'
		for l in textwrap.wrap(output, width):
			tfile.write(prefix + l + '\n')
	skip = True

tfile.close()
compare_srcfile(tmp_file, f)

#####################################################################
# Create config_def.c with defaults for each config string
#####################################################################
f='../src/api/config_def.c'
tfile = open(tmp_file, 'w')

tfile.write('''/* DO NOT EDIT: automatically built by dist/config.py. */

#include "wt_internal.h"
''')

# Make a TextWrapper that can wrap at commas.
w = textwrap.TextWrapper(width=75)
w.wordsep_re = w.wordsep_simple_re = re.compile(r'(,)')

for name in sorted(config_data.config_types.keys()):
	ctype = config_data.config_types[name]
	tfile.write('''
const char *
__wt_config_def_%(name)s =
%(config)s;
''' % {
	'name' : name,
	'config' : '\n'.join('    "%s"' % line
		for line in w.wrap(','.join('%s=%s' % (c.name, c.default)
			for c in ctype)) or [""]),
})

tfile.close()
compare_srcfile(tmp_file, f)
