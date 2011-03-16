# Output serialization #defines.

from dist import compare_srcfile

serial = {}
class Serial:
	def __init__(self, key, op, spin, args):
		self.key = key
		self.op = op
		self.spin = spin
		self.args = args

serial['cache_read'] = Serial(
	'cache_read',
	'WT_WORKQ_READ', '0',
	['WT_PAGE */parent',
	 'void */parent_ref',
	 'int/dsk_verify'])

serial['insert'] = Serial(
	'insert',
	'WT_WORKQ_FUNC', '1',
	['WT_PAGE */page',
	 'uint32_t/write_gen',
	 'WT_INSERT **/new_ins',
	 'WT_INSERT **/srch_ins',
	 'WT_INSERT */ins'])

serial['key_build'] = Serial(
	'key_build',
	'WT_WORKQ_FUNC', '0',
	['void */key_arg',
	 'WT_ITEM */item'])

serial['update'] = Serial(
	'update',
	'WT_WORKQ_FUNC', '1',
	['WT_PAGE */page',
	 'uint32_t/write_gen',
	 'WT_UPDATE **/new_upd',
	 'WT_UPDATE **/srch_upd',
	 'WT_UPDATE */upd'])

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
		f.write('#define\t__wt_' + entry[0] + '_serial(\\\n    session')
		for l in entry[1].args:
			f.write(', _' + l.split('/')[1])
		f.write(', ret) do {\\\n')
		f.write('\t__wt_' + entry[0] + '_args _args;\\\n')
		for l in entry[1].args:
			f.write('\t_args.' + l.split('/')[1] +
			    ' = _' + l.split('/')[1] + ';\\\n')
		f.write('\t(ret) = __wt_session_serialize_func(session,\\\n')
		f.write('\t    ' + entry[1].op + ', ' + entry[1].spin +
		    ', __wt_' + entry[1].key + '_serial_func, &_args);\\\n')
		f.write('} while (0)\n')

		# unpack function
		f.write('#define\t__wt_' + entry[0] + '_unpack(\\\n    session')
		for l in entry[1].args:
			f.write(', _' + l.split('/')[1])
		f.write(') do {\\\n')
		f.write('\t__wt_' + entry[0] + '_args *_args =\\\n	    ')
		f.write('(__wt_' + entry[0] + '_args *)(session)->wq_args;\\\n')
		for l in entry[1].args:
			f.write('\t_' + l.split('/')[1] +
			    ' = _args->' + l.split('/')[1] + ';\\\n')
		f.write('} while (0)\n')

#####################################################################
# Update serial.h.
#####################################################################
tmp_file = '__tmp'
tfile = open(tmp_file, 'w')
tfile.write('/* DO NOT EDIT: automatically built by dist/serial.py. */\n')
func_serial(tfile)
tfile.close()
compare_srcfile(tmp_file, '../src/include/serial.h')
