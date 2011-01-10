# $Id$

# Output serialization #defines.

import os
from dist import compare_srcfile

serial = {}
class Serial:
	def __init__(self, key, op, spin, args):
		self.key = key
		self.op = op
		self.spin = spin
		self.args = args

serial['bt_rcc_expand'] = Serial(
	'bt_rcc_expand',
	'WT_WORKQ_FUNC', '1',
	['WT_PAGE */page',
	 'uint32_t/write_gen',
	 'uint32_t/slot',
	 'WT_RCC_EXPAND **/new_rccexp',
	 'WT_RCC_EXPAND */exp'])

serial['bt_rcc_expand_repl'] = Serial(
	'bt_rcc_expand_repl',
	'WT_WORKQ_FUNC', '1',
	['WT_PAGE */page',
	 'uint32_t/write_gen',
	 'WT_RCC_EXPAND */exp',
	 'WT_REPL */repl'])

serial['bt_item_update'] = Serial(
	'bt_item_update',
	'WT_WORKQ_FUNC', '1',
	['WT_PAGE */page',
	 'uint32_t/write_gen',
	 'uint32_t/slot',
	 'WT_REPL **/new_repl',
	 'WT_REPL */repl'])

serial['cache_read'] = Serial(
	'cache_read',
	'WT_WORKQ_READ', '0',
	['WT_REF */ref',
	 'WT_OFF */off',
	 'int/dsk_verify'])

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
		f.write('#define\t__wt_' + entry[0] + '_serial(\\\n    toc')
		for l in entry[1].args:
			f.write(', _' + l.split('/')[1])
		f.write(', ret) do {\\\n')
		f.write('\t__wt_' + entry[0] + '_args _args;\\\n')
		for l in entry[1].args:
			f.write('\t_args.' + l.split('/')[1] +
			    ' = _' + l.split('/')[1] + ';\\\n')
		f.write('\t(ret) = __wt_toc_serialize_func(toc,\\\n')
		f.write('\t    ' + entry[1].op + ', ' + entry[1].spin +
		    ', __wt_' + entry[1].key + '_serial_func, &_args);\\\n')
		f.write('} while (0)\n')

		# unpack function
		f.write('#define\t__wt_' + entry[0] + '_unpack(\\\n    toc')
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
# Update serial.h.
#####################################################################
tmp_file = '__tmp'
tfile = open(tmp_file, 'w')
tfile.write('/* DO NOT EDIT: automatically built by dist/serial.py. */\n')
func_serial(tfile)
tfile.close()
compare_srcfile(tmp_file, '../inc_posix/serial.h')

os.remove(tmp_file)
