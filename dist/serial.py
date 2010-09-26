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
class Serial:
	def __init__(self, key, op, args):
		self.key = key
		self.op = op
		self.args = args

serial['bt_rcc_expand'] = Serial(
	'bt_rcc_expand',
	'WT_WORKQ_SPIN',
	['WT_PAGE */page',
	 'u_int16_t/write_gen',
	 'int/slot',
	 'WT_COL_EXPAND **/new_expcol',
	 'WT_COL_EXPAND */exp'])

serial['bt_rcc_expand_repl'] = Serial(
	'bt_rcc_expand_repl',
	'WT_WORKQ_SPIN',
	['WT_PAGE */page',
	 'u_int16_t/write_gen',
	 'WT_COL_EXPAND */exp',
	 'WT_REPL */repl'])

serial['bt_update'] = Serial(
	'bt_update',
	'WT_WORKQ_SPIN',
	['WT_PAGE */page',
	 'u_int16_t/write_gen',
	 'int/slot',
	 'WT_REPL **/new_repl',
	 'WT_REPL */repl'])

serial['cache_in'] = Serial(
	'cache_in',
	'WT_WORKQ_READ',
	['u_int32_t/addr',
	 'u_int32_t/size',
	 'WT_PAGE **/pagep'])

# func_serial --
#	Loop through the serial dictionary and output #defines to schedule
#	work for the workQ thread.
def func_serial(f):
	for entry in sorted(serial.items()):
		# structure declaration
		f.write('\n')
		f.write('typedef struct {\n')
		for l in entry[1].args:
			f.write('\t' + l.replace('/', ' ')  + ';\n')
		f.write('} __wt_' + entry[0] + '_args;\n')

		# pack function
		f.write('#define\t __wt_' + entry[0] + '_serial(toc')
		for l in entry[1].args:
			f.write(', _' + l.split('/')[1])
		f.write(', ret) do {\\\n')
		f.write('\t__wt_' + entry[0] + '_args _args;\\\n')
		for l in entry[1].args:
			f.write('\t_args.' + l.split('/')[1] +
			    ' = _' + l.split('/')[1] + ';\\\n')
		f.write('\t(ret) = __wt_toc_serialize_func(\\\n')
		f.write('\t    toc, ' + entry[1].op + ', __wt_' +
		    entry[1].key + '_serial_func, &_args);\\\n')
		f.write('} while (0)\n')

		# unpack function
		f.write('#define\t__wt_' + entry[0] + '_unpack(toc')
		for l in entry[1].args:
			f.write(', _' + l.split('/')[1])
		f.write(') do {\\\n')
		for l in entry[1].args:
			f.write('\t_' + l.split('/')[1] +
			    ' = ((__wt_' + entry[0] +
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
