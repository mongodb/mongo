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
	if config_name not in api_data.methods:
		print >>sys.stderr, "Missing configuration for " + config_name
		continue

	width = 80 - len(prefix.expandtabs())
	for c in sorted(api_data.methods[config_name].config):
		desc = textwrap.dedent(c.desc).replace(',', '\\,')
		name = c.name
		if '.' in name:
			print >>sys.stderr, "Bad config key " + name
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
w = textwrap.TextWrapper(width=72)
w.wordsep_re = w.wordsep_simple_re = re.compile(r'(,)')

def checkstr(c):
	'''Generate the JSON string used by __wt_config_check to validate the
	config string'''
	checks = c.flags
	ctype = checks.get('type', None)
	cmin = str(checks.get('min', ''))
	cmax = str(checks.get('max', ''))
	choices = checks.get('choices', [])
	if not ctype and ('min' in checks or 'max' in checks):
		ctype = 'int'
	result = []
	if ctype:
		result.append('type=' + ctype)
	if cmin:
		result.append('min=' + cmin)
	if cmax:
		result.append('max=' + cmax)
	if choices:
		result.append('choices=' + '[' +
		    ','.join('\\"' + s + '\\"' for s in choices) + ']')
	return ','.join(result)

for name in sorted(api_data.methods.keys()):
	ctype = api_data.methods[name].config
	name = name.replace('.', '_')
	tfile.write('''
const char *
__wt_confdfl_%(name)s =
%(config)s;
''' % {
	'name' : name,
	'config' : '\n'.join('    "%s"' % line
		for line in w.wrap(','.join('%s=%s' % (c.name, c.default)
			for c in sorted(ctype))) or [""]),
})
	tfile.write('''
const char *
__wt_confchk_%(name)s =
%(check)s;
''' % {
	'name' : name,
	'check' : '\n'.join('    "%s"' % line
		for line in w.wrap(','.join('%s=(%s)' % (c.name, checkstr(c))
			for c in sorted(ctype))) or [""]),
})

tfile.close()
compare_srcfile(tmp_file, f)
