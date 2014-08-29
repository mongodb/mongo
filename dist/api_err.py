# Output C #defines for errors into wiredtiger.in and the associated error
# message code in strerror.c.

import re, textwrap

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
		for err in api_data.errors:
			if 'undoc' in err.flags:
				tfile.write('/*! @cond internal */\n')
			tfile.write('/*!%s.%s */\n' %
			    (('\n * ' if err.long_desc else ' ') +
                    err.desc[0].upper() + err.desc[1:],
			    ''.join('\n * ' + l for l in textwrap.wrap(
                    textwrap.dedent(err.long_desc).strip(), 77)) +
                '\n' if err.long_desc else ''))
			tfile.write('#define\t%s\t%d\n' % (err.name, v))
			v -= 1
			if 'undoc' in err.flags:
				tfile.write('/*! @endcond */\n')
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

for err in api_data.errors:
	tfile.write('\tcase ' + err.name + ':\n')
	tfile.write('\t\treturn ("' + err.name + ': ' + err.desc + '");\n')

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
compare_srcfile(tmp_file, '../src/conn/api_strerror.c')

# Update the error documentation block.
doc = '../src/docs/error-handling.dox'
tmp_file = '__tmp'
tfile = open(tmp_file, 'w')
skip = 0
for line in open(doc, 'r'):
	if not skip:
		tfile.write(line)
	if line.count('IGNORE_BUILT_BY_API_ERR_END'):
		tfile.write(line)
		skip = 0
	elif line.count('IGNORE_BUILT_BY_API_ERR_BEGIN'):
		tfile.write('@endif\n\n')
		skip = 1

		for err in api_data.errors:
			if 'undoc' in err.flags:
				continue
			tfile.write(
			    '@par <code>' + err.name.upper() + '</code>\n' +
			    " ".join(err.long_desc.split()) + '\n\n')
tfile.close()
compare_srcfile(tmp_file, doc)
