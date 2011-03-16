#!/usr/bin/env python

import os, re, sys, textwrap
import log_data
from dist import compare_srcfile

# Temporary file.
tmp_file = '__tmp'

# Map log record types to C
c_types = {
		'string' : 'const char *',
}

# Map log record types to format strings
fmt_types = {
		'string' : 'S',
}

#####################################################################
# Create log.i with inline functions for each log record type.
#####################################################################
f='../src/include/log.i'
tfile = open(tmp_file, 'w')

tfile.write('/* DO NOT EDIT: automatically built by dist/log.py. */\n')

for t in log_data.types:
	tfile.write('''
static inline int
__wt_logput_%(name)s(SESSION *session, %(param_decl)s)
{
	return (__wt_log_put(session, &__wt_logdesc_%(name)s, %(param_list)s));
}
''' % {
	'name' : t.name,
	'param_decl' : ', '.join(
	    '%s %s' % (c_types.get(t, t), n) for t, n in t.fields),
	'param_list' : ', '.join(n for t, n in t.fields),
})

tfile.close()
compare_srcfile(tmp_file, f)

#####################################################################
# Create log_desc.c with descriptors for each log record type.
#####################################################################
f='../src/log/log_desc.c'
tfile = open(tmp_file, 'w')

tfile.write('''/* DO NOT EDIT: automatically built by dist/log.py. */

#include "wt_internal.h"
''')

for t in log_data.types:
	tfile.write('''
WT_LOGREC_DESC
__wt_logdesc_%(name)s =
{
	"%(fmt)s", { %(field_list)s, NULL }
};
''' % {
	'name' : t.name,
	'fmt' : ''.join(fmt_types[t] for t, n in t.fields),
	'field_list' : ', '.join('"%s"' % n for t, n in t.fields),
})

tfile.close()
compare_srcfile(tmp_file, f)
