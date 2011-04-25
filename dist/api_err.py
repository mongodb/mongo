# Output C #defines for errors into wiredtiger.in and the associated error
# message code in strerror.c.

import re

import api_data
from dist import compare_srcfile

# Update the #defines in the wiredtiger.in file.
tmp_file = '__tmp'
tfile = open(tmp_file, 'w')
skip = 0
for line in open('../src/include/wiredtiger.in', 'r'):
	if not skip:
		tfile.write(line)
	if line.count('Error return section: END'):
		tfile.write(line)
		skip = 0
	elif line.count('Error return section: BEGIN'):
		tfile.write(' */\n')
		skip = 1

		# We don't want our error returns to conflict with any other
		# package, so use an uncommon range, specifically, -31,800 to
		# -31,999.
		v = -31800
		for name, msg in api_data.errors:
			tfile.write('/*! %s. */\n' % (msg[0].upper() + msg[1:]))
			tfile.write('#define\t%s\t%d\n' % (name, v))
			v -= 1
		tfile.write('/*\n')
tfile.close()
compare_srcfile(tmp_file, '../src/include/wiredtiger.in')

# Output the wiredtiger_strerror code.
tmp_file = '__tmp'
tfile = open(tmp_file, 'w')
tfile.write('''/* DO NOT EDIT: automatically built by dist/api_err.py. */

#include "wt_internal.h"

/*
 * wiredtiger_strerror --
 *	Return a string for any error value.
 */
const char *
wiredtiger_strerror(int error)
{
	static char errbuf[64];
	char *p;

	if (error == 0)
		return ("Successful return: 0");

	switch (error) {
''')

for l in api_data.errors:
	tfile.write('\tcase ' + l[0] + ':\n')
	tfile.write('\t\treturn ("' + l[0] + ': ' + l[1] + '");\n')

tfile.write('''\
	default:
		if (error > 0 && (p = strerror(error)) != NULL)
			return (p);
		break;
	}

	/*
	 * !!!
	 * Not thread-safe, but this is never supposed to happen.
	 */
	(void)snprintf(errbuf, sizeof(errbuf), "Unknown error: %d", error);
	return (errbuf);
}
''')
tfile.close()
compare_srcfile(tmp_file, '../src/api/strerror.c')
