# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008 WiredTiger Software.
#	All rights reserved.
#
# $Id$

# Output serialization #defines.

import os
from dist import compare_srcfile

serial = {}
serial['bt_del'] = [
	'WT_REPL */new']

serial['bt_repl'] = [
	'WT_ROW_INDX */indx',
	'WT_REPL */repl',
	'void */data',
	'u_int32_t/size']

serial['flist_free'] = [
	'WT_FLIST */fp']

# func_serial --
#	Loop through the serial dictionary and output #defines to schedule
#	work for the workQ thread.
def func_serial(f):
	for entry in sorted(serial.iteritems()):
		# structure declaration
		f.write('\n')
		f.write('typedef struct {\n')
		for l in entry[1]:
			f.write('\t' + l.replace('/', ' ') + ';\n')
		f.write('} __wt_' + entry[0] + '_args;\n')

		# pack function
		f.write('#define\t __wt_' + entry[0] + '_serial(toc')
		for l in entry[1]:
			f.write(', _' + l.split('/')[1])
		f.write(', ret) do {\\\n')
		f.write('\t__wt_' + entry[0] + '_args _args;\\\n')
		for l in entry[1]:
			f.write('\t_args.' + l.split('/')[1] +
			    ' = _' + l.split('/')[1] + ';\\\n')
		f.write('\t(ret) = __wt_toc_serialize_func(\\\n')
		f.write('\t    toc, __wt_' +
		    entry[0] + '_serial_func, &_args);\\\n')
		f.write('} while (0)\n')

		# unpack function
		f.write('#define\t__wt_' + entry[0] + '_unpack(toc')
		for l in entry[1]:
			f.write(', _' + l.split('/')[1])
		f.write(') do {\\\n')
		for l in entry[1]:
			f.write('\t_' + l.split('/')[1] + ' =\\\n')
			f.write('\t    ((__wt_' + entry[0] +
			    '_args *)(toc)->wq_args)->' +
			    l.split('/')[1] + ';\\\n')
		f.write('} while (0)\n')

#####################################################################
# Update serial.h, the serialization header file.
#####################################################################
tmp_file = '__tmp'
tfile = open(tmp_file, 'w')
tfile.write('/* DO NOT EDIT: automatically built by dist/serial.py. */\n')
func_serial(tfile)
tfile.close()
compare_srcfile(tmp_file, '../inc_posix/serial.h')

os.remove(tmp_file)
