#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#

# wtperf.py
# A partial emulation of wtperf. Translates a .wtperf file into a Python
# script that uses the workgen module, and runs the script. Errors are
# issued for any .wtperf directives that are not known.
# See also the usage() function.
#
from __future__ import print_function
import os, shutil, sys, subprocess, tempfile

def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)

class OptionValue:
    def __init__(self, value, filename, linenum):
        self.value = value
        self.filename = filename
        self.linenum = linenum

class TranslateException(Exception):
    pass

class Options(object):
    pass

class Translator:
    def __init__(self, filename, prefix, verbose, homedir):
        self.filename = filename
        self.prefix = prefix
        self.verbose = verbose
        self.homedir = homedir
        self.linenum = 0
        self.opts_map = {}
        self.opts_used = {}
        self.options = lambda: None   # options behaves as an attribute dict
        self.has_error = False

    def error_file_line(self, fname, linenum, msg):
        self.has_error = True
        eprint(fname + ':' + str(linenum) + ': error: ' + msg)

    # Report an error and continue
    def error(self, msg):
        self.error_file_line(self.filename, self.linenum, msg)

    # Report an error and unwind the stack
    def fatal_error(self, msg, errtype = 'configuration error'):
        self.error(msg)
        raise TranslateException(errtype)

    supported_opt_list = [ 'checkpoint_interval', 'checkpoint_threads',
                           'close_conn', 'compact', 'compressibility',
                           'compression', 'conn_config', 'create', 'icount',
                           'key_sz', 'log_like_table', 'pareto',
                           'populate_ops_per_txn', 'populate_threads',
                           'random_range', 'random_value', 'range_partition',
                           'readonly', 'reopen_connection', 'run_ops',
                           'sample_interval', 'sess_config', 'table_config',
                           'table_count', 'threads', 'transaction_config',
                           'value_sz',
                           'max_idle_table_cycle',
                           'max_idle_table_cycle_fatal' ]

    def set_opt(self, optname, val):
        if optname not in self.supported_opt_list:
            self.error("unknown option: " + optname)
            return
        elif val[0] == '"' and val[-1] == '"':
            v = val[1:-1]
        elif val == 'true':
            v = True
        elif val == 'false':
            v = False
        elif val[0] == '(':
            v = val      # config string stored as is
        else:
            try:
                v = int(val)   # it might be an integer
            except ValueError:
                v = val        # it's a string after all
        self.opts_map[optname] = OptionValue(v, self.filename, self.linenum)

    def _get_opt(self, optname, dfault):
        if optname in self.opts_map:
            ret = self.opts_map[optname]
            self.filename = ret.filename
            self.linenum = ret.linenum
            self.opts_used[optname] = 1
            return ret.value
        else:
            return dfault

    def get_string_opt(self, optname, dfault):
        v = self._get_opt(optname, dfault)
        setattr(self.options, optname, v)
        return v

    def get_int_opt(self, optname, dfault):
        v = self._get_opt(optname, dfault) + 0
        setattr(self.options, optname, v)
        return v

    # Convert a time value, by default a number of seconds, that can be
    # modified to microseconds using 'ms' as a suffix.
    def get_intms_opt(self, optname, wtperf_optname, dfault):
        s = str(self._get_opt(wtperf_optname, str(dfault)))
        if s.endswith('ms'):
            v = int(s[:-2])
        else:
            v = 1000 * int(s)
        setattr(self.options, optname, v)
        return v

    def get_boolean_opt(self, optname, dfault):
        v = not not self._get_opt(optname, dfault)
        setattr(self.options, optname, v)
        return v

    # Split a string 'left_side=right_side' into two parts
    def split_assign(self, s):
        equalpos = s.find('=')
        if equalpos < 0:
            self.error("missing '=' for line: " + line)
            return (None, None)
        else:
            return s.split('=', 1)

    # Split a config string honoring nesting e.g.
    # "(abc=123,def=234,ghi=(hi=1,bye=2))" would return 3 items.
    def split_config_parens(self, s):
        if s[0:1] != '(':
            self.fatal_error('missing left paren', 'config parse error')
        if s[-1:] != ')':
            self.fatal_error('missing right paren', 'config parse error')
        s = s[1:-1]
        result = []
        level = 0
        cur = ''
        for ch in s:
            if ch == ',' and level == 0:
                result.append(cur)
                cur = ''
            else:
                cur += ch
            if ch == '(':
                level += 1
            elif ch == ')':
                level -= 1
                if level < 0:
                    self.fatal_error('unbalanced paren', 'config parse error')
        if level != 0:
            self.fatal_error('unbalanced paren', 'config parse error')
        if len(cur) != 0:
            result.append(cur)
        return result

    def assign_str(self, left, right):
        return left + '=' + str(right) + '\n'

    def add_operation_str(self, count, opname, multi, pareto):
        result = ''
        tablename = 'tables[0]' if multi else 'table'
        if count > 1:
            result += str(count) + ' * '
        if count > 0:
            result += 'Operation(Operation.' + opname + ', ' + tablename
            if pareto > 0:
                result += ', Key(Key.KEYGEN_PARETO, 0, ParetoOptions(' + \
                          str(pareto) + '))'
            elif opname == 'OP_INSERT' and self.options.random_range != 0:
                result += ', Key(Key.KEYGEN_UNIFORM)'
            result += ') + \\\n'
            result += '      '
        return result

    def copy_file(self, srcname, destdir, destname):
        dest_fullname = os.path.join(destdir, destname)
        suffix = 0
        while os.path.exists(dest_fullname):
            suffix += 1
            dest_fullname = os.path.join(destdir, destname + str(suffix))
        shutil.copyfile(srcname, dest_fullname)

    def copy_config(self):
        # Note: If we add the capability of setting options on the command
        # line, we won't be able to do a simple copy.
        self.copy_file(self.filename, self.homedir, 'CONFIG.wtperf')

    def copy_python_source(self, srcname):
        self.copy_file(srcname, self.homedir, 'RUN.py')

    # Wtperf's throttle is based on the number of regular operations,
    # not including log_like operations.  Workgen counts all operations,
    # it doesn't treat log operations any differently.
    def calc_throttle(self, thread_opts, log_like_table):
        throttle = thread_opts.throttle
        comment = ''
        factor = 1.0
        if log_like_table:
            modify = thread_opts.inserts + thread_opts.updates
            regular = modify + thread_opts.reads
            total = regular + modify
            factor = (total + 0.0) / regular
        if factor != 1.0:
            comment = \
                '# These operations include log_like operations, which ' + \
                'will increase the number\n# of insert/update operations ' + \
                'by a factor of ' + str(factor) + '. This may cause the\n' + \
                '# actual operations performed to be above the throttle.\n'
        return (throttle, comment)

    def parse_threads(self, threads_config, checkpoint_threads):
        opts = self.options
        tdecls = ''
        tlist = self.split_config_parens(threads_config)
        table_count = self.get_int_opt('table_count', 1)
        log_like_table = self.get_boolean_opt('log_like_table', False)
        txn_config = self.get_string_opt('transaction_config', '')
        checkpoint_interval = self.get_int_opt('checkpoint_interval', 120)
        run_ops = self.get_int_opt('run_ops', -1)
        if log_like_table:
            tdecls += '# Log like file, requires that logging be enabled ' + \
                      'in the connection config.\n'
            tdecls += 'log_name = "table:log"\n'
            tdecls += 's.create(log_name, wtperf_table_config +' \
                      ' "key_format=S,value_format=S," +' + \
                      ' compress_table_config + table_config +' \
                      ' ",log=(enabled=true)")\n'
            tdecls += 'log_table = Table(log_name)\n'
            if opts.compressibility != 100:
                tdecls += 'log_table.options.value_compressibility = ' + \
                str(opts.compressibility) + '\n'
            tdecls += '\n'

        thread_count = 0
        tnames = ''
        multi = (table_count > 1)
        for t in tlist:
            thread_name = 'thread' + str(thread_count)
            thread_count += 1

            # For wtperf compatibility, we allow both 'insert/inserts' etc.
            topts = Options()
            topts.count = 1
            topts.insert = 0
            topts.inserts = 0
            topts.ops_per_txn = 0
            topts.read = 0
            topts.reads = 0
            topts.throttle = 0
            # Workgen's throttle_burst variable has a default of 1.0 .  Since we
            # are always explicitly setting it, set our own value to the same.
            topts.throttle_burst = 1.0
            topts.update = 0
            topts.updates = 0
            topts.random_range = 0

            for o in self.split_config_parens(t):
                (k, v) = self.split_assign(o)
                if hasattr(topts, k):
                    try:
                        setattr(topts, k, int(v))
                    except ValueError:
                        self.error('thread option ' + k + ': integer expected')
                else:
                    self.error('unknown thread option: ' + k)

            topts.inserts += topts.insert; topts.insert = 0
            topts.updates += topts.update; topts.update = 0
            topts.reads += topts.read; topts.read = 0
            if topts.count == 0:
                continue

            if topts.inserts + topts.reads + topts.updates == 0:
                self.fatal_error('need read/insert/update/...',
                                 'thread config error')

            tdecls += 'ops = '
            tdecls += self.add_operation_str(topts.inserts, 'OP_INSERT',
                multi, opts.pareto)
            tdecls += self.add_operation_str(topts.reads, 'OP_SEARCH',
                multi, opts.pareto)
            tdecls += self.add_operation_str(topts.updates, 'OP_UPDATE',
                multi, opts.pareto)
            tdecls = tdecls.rstrip(' \n\\+') + '\n'
            range_partition = opts.range_partition

            # Pareto with multiple tables is handled in op_multi_table.
            if multi:
                tdecls += 'ops = op_multi_table(ops, tables, ' + \
                          str(range_partition) + ')\n'
            if topts.ops_per_txn > 0:
                tdecls += 'ops = op_group_transaction(ops, ' + \
                          str(topts.ops_per_txn) + ', "' + txn_config + '")\n'
            if log_like_table:
                tdecls += 'ops = op_log_like(ops, log_table, ' + \
                          str(topts.ops_per_txn) + ')\n'
            if run_ops != -1:
                if len(tlist) > 1:
                    self.fatal_error('run_ops currently supported with a '
                                     'single type of thread')
                tdecls += '\n'
                if multi:
                    tdecls += \
                        '# Note that op_multi_table has already multiplied\n' +\
                        '# the number of operations by the number of tables.\n'
                tdecls += 'ops = ops * (' + \
                          str(run_ops) + ' // (' + str(topts.count) + \
                          ' * table_count))' + \
                          '     # run_ops = ' + str(run_ops) + \
                          ', thread.count = ' + str(topts.count) + '\n'
            tdecls += thread_name + ' = Thread(ops)\n'
            if topts.throttle > 0:
                (throttle, comment) = self.calc_throttle(topts, log_like_table)
                tdecls += comment
                tdecls += self.assign_str(
                    thread_name + '.options.throttle', throttle)
                tdecls += self.assign_str(
                    thread_name + '.options.throttle_burst',
                    topts.throttle_burst)
            tdecls += '\n'
            if topts.count > 1:
                tnames += str(topts.count) + ' * '
            tnames += thread_name + ' + '

        if checkpoint_threads != 0:
            thread_name = 'checkpoint_thread'

            tdecls += 'ops = Operation(Operation.OP_SLEEP, "' + \
                str(checkpoint_interval) + \
                '") + \\\n      Operation(Operation.OP_CHECKPOINT, "")\n'
            tdecls += thread_name + ' = Thread(ops)\n'
            tdecls += '\n'
            if checkpoint_threads > 1:
                tnames += str(checkpoint_threads) + ' * '
            tnames += thread_name + ' + '

        if log_like_table:
            thread_name = 'logging_thread'

            tdecls += 'ops = Operation(Operation.OP_SLEEP, "0.1") + \\\n' + \
                '      Operation(Operation.OP_LOG_FLUSH, "")\n'
            tdecls += thread_name + ' = Thread(ops)\n'
            tdecls += '\n'
            tnames += thread_name + ' + '

        tnames = tnames.rstrip(' +')
        return (tdecls, tnames)

    def translate(self):
        try:
            return self.translate_inner()
        except TranslateException:
            # An error has already been reported
            return None

    def check_divisibility(self, icount, random_range, divisor_name, divisor):
        if (icount + random_range) % divisor != 0:
            if random_range == 0:
                dividend = 'icount'
            else:
                dividend = '(icount + random_range)'
                self.fatal_error(dividend + ' is not evenly divisible by ' +
                                 divisor_name + ', this is not handled ' +
                                 'precisely by wtperf.py')

    def translate_table_create(self):
        opts = self.options
        s = ''
        s += 'wtperf_table_config = "key_format=S,value_format=S," +\\\n'
        s += '    "exclusive=true,allocation_size=4kb," +\\\n'
        s += '    "internal_page_max=64kb,leaf_page_max=4kb,split_pct=100,"\n'
        if opts.compression != '':
            s += 'compress_table_config = "block_compressor=' + opts.compression + ',"\n'
        else:
            s += 'compress_table_config = ""\n'
        s += 'table_config = "' + opts.table_config + '"\n'
        s += 'tables = []\n'
        s += 'table_count = ' + str(opts.table_count) + '\n'
        if opts.table_count == 1:
            s += 'tname = "table:test"\n'
            indent = ''
        else:
            s += 'for i in range(0, table_count):\n'
            s += '    tname = "table:test" + str(i)\n'
            indent = '    '

        s += indent + 'table = Table(tname)\n'
        s += indent + 's.create(tname, wtperf_table_config +\\\n'
        s += indent + '         compress_table_config + table_config)\n'
        s += indent + 'table.options.key_size = ' + str(opts.key_sz) + '\n'
        s += indent + 'table.options.value_size = ' + str(opts.value_sz) + '\n'
        if opts.random_value:
            s += indent + 'table.options.random_value = True\n'
        if opts.random_range != 0:
            # In wtperf, the icount plus random_range is the key range
            table_range = (opts.random_range + opts.icount) // opts.table_count
            s += indent + 'table.options.range = ' + str(table_range) + '\n'
        if opts.compressibility != 100:
            s += indent + 'table.options.value_compressibility = ' + \
                str(opts.compressibility) + '\n'
        s += indent + 'tables.append(table)\n'
        return s

    def translate_populate(self):
        opts = self.options
        s = '\n'
        if opts.icount == 0:
            if opts.populate_threads != 0:
                self.error("populate_threads > 0, icount == 0")
            return ''
        if opts.populate_threads == 0:
            self.fatal_error('icount != 0 and populate_threads == 0: ' +\
                             'cannot populate entries with no threads')
        s += 'populate_threads = ' + str(opts.populate_threads) + '\n'
        s += 'icount = ' + str(opts.icount) + '\n'
        need_ops_per_thread = True

        # Since we're separating the populating by table, and also
        # into multiple threads, we currently require that
        # (icount + random_range) is evenly divisible by table count
        # and by number of populating threads.  It's possible to handle
        # the cases when this is not true, but it hardly seems worth
        # the extra complexity.  Also, these could be made into warnings,
        # and actually create fewer entries than icount, but that could be
        # confusing.
        self.check_divisibility(opts.icount, opts.random_range,
                                'table_count', opts.table_count)
        self.check_divisibility(opts.icount, opts.random_range,
                                '(populate_threads * table_count)',
                                opts.populate_threads * opts.table_count)

        if opts.table_count == 1:
            s += 'pop_ops = Operation(Operation.OP_INSERT, table)\n'
        elif opts.range_partition and opts.random_range > 0:
            # Populating using a range partition is complex enough
            # to handle in its own function.  It does all the operations
            # for the thread, so we don't need a multiplier at the end.
            need_ops_per_thread = False

            s += 'random_range = ' + str(opts.random_range) + '\n'
            s += 'pop_ops = Operation(Operation.OP_INSERT, tables[0])\n'
            s += 'pop_ops = op_populate_with_range(pop_ops, tables, ' + \
                 'icount, random_range, populate_threads)\n'
        else:
            s += '# There are multiple tables to be filled during populate,\n'
            s += '# the icount is split between them all.\n'
            s += 'pop_ops = Operation(Operation.OP_INSERT, tables[0])\n'
            s += 'pop_ops = op_multi_table(pop_ops, tables)\n'

        if need_ops_per_thread:
            s += 'nops_per_thread = icount // (populate_threads * table_count)\n'
            op_mult = ' * nops_per_thread'
        else:
            op_mult = ''

        pop_per_txn = opts.populate_ops_per_txn
        if pop_per_txn > 0:
            s += 'pop_ops = op_group_transaction(pop_ops, ' + \
                 str(pop_per_txn) + ', "' + opts.transaction_config + '")\n'
        s += 'pop_thread = Thread(pop_ops' + op_mult + ')\n'
        s += 'pop_workload = Workload(context, populate_threads * pop_thread)\n'
        if self.verbose > 0:
            s += 'print("populate:")\n'
        s += 'ret = pop_workload.run(conn)\n'
        s += 'assert ret == 0, ret\n'

        return s

    def translate_inner(self):
        workloadopts = ''
        input_as_string = ''
        with open(self.filename) as fin:
            for line in fin:
                self.linenum += 1
                input_as_string += line
                commentpos = line.find('#')
                if commentpos >= 0:
                    line = line[0:commentpos]
                line = line.strip()
                if len(line) == 0:
                    continue
                (key, val) = self.split_assign(line)
                if key in [ 'max_latency', 'report_file', 'report_interval',
                            'run_time', 'sample_rate',
                            'warmup' ]:
                    workloadopts += 'workload.options.' + key + '=' + val + '\n'
                else:
                    self.set_opt(key, val)

        conn_config = self.get_string_opt('conn_config', '')
        sess_config = self.get_string_opt('sess_config', '')
        create = self.get_boolean_opt('create', True)
        reopen_connection = self.get_boolean_opt('reopen_connection', False)
        readonly = self.get_boolean_opt('readonly', False)
        close_conn = self.get_boolean_opt('close_conn', True)
        compression = self.get_string_opt('compression', '')
        self.get_intms_opt('sample_interval_ms', 'sample_interval', 0)
        self.get_int_opt('compressibility', 100)
        self.get_int_opt('table_count', 1)
        self.get_string_opt('table_config', '')
        self.get_int_opt('key_sz', 20)
        self.get_int_opt('value_sz', 100)
        self.get_int_opt('icount', 0)
        self.get_int_opt('populate_threads', 1)
        self.get_int_opt('populate_ops_per_txn', 0)
        self.get_boolean_opt('range_partition', False)
        self.get_int_opt('random_range', 0)
        self.get_boolean_opt('random_value', False)
        self.get_string_opt('transaction_config', '')
        self.get_boolean_opt('compact', False)
        self.get_int_opt('pareto', 0)
        self.get_int_opt('max_idle_table_cycle', 0)
        self.get_boolean_opt('max_idle_table_cycle_fatal', False)
        opts = self.options
        if opts.range_partition and opts.random_range == 0:
            self.fatal_error('range_partition requires random_range to be set')
        if opts.random_range > 0 and not opts.range_partition and \
           opts.table_count != 1:
            self.fatal_error('random_range and multiple tables without ' + \
                             'range_partition is not supported')

        if self.options.sample_interval_ms != 0:
            workloadopts += 'workload.options.sample_interval_ms = ' + \
                str(self.options.sample_interval_ms) + '\n'

        if self.options.max_idle_table_cycle > 0:
            workloadopts += 'workload.options.max_idle_table_cycle = ' + \
            str(self.options.max_idle_table_cycle) + '\n'

        if self.options.max_idle_table_cycle_fatal:
            workloadopts += 'workload.options.max_idle_table_cycle_fatal = ' + \
            str(self.options.max_idle_table_cycle_fatal) + '\n'

        s = '#/usr/bin/env python\n'
        s += '# generated from ' + self.filename + '\n'
        s += self.prefix
        s += 'from runner import *\n'
        s += 'from wiredtiger import *\n'
        s += 'from workgen import *\n'
        s += '\n'
        s += '\'\'\' The original wtperf input file follows:\n'
        s += input_as_string
        if not input_as_string.endswith('\n'):
            s += '\n'
        s += '\'\'\'\n\n'
        s += 'context = Context()\n'
        extra_config = ''
        s += 'conn_config = ""\n'

        if conn_config != '':
            s += 'conn_config += ",' + conn_config + '"   # explicitly added\n'
        if compression != '':
            # We require WiredTiger to be configured with snappy built-in,
            # so do not add snappy to the list of extensions to be loaded.
            if compression != 'snappy':
                s += 'conn_config += extensions_config(["compressors/' + \
                    compression + '"])\n'
            compression = 'block_compressor=' + compression + ','
        s += 'conn = context.wiredtiger_open("create," + conn_config)\n'
        s += 's = conn.open_session("' + sess_config + '")\n'
        s += '\n'
        s += self.translate_table_create()
        if create:
            s += self.translate_populate()

        thread_config = self.get_string_opt('threads', '')
        checkpoint_threads = self.get_int_opt('checkpoint_threads', 0)
        if thread_config != '' or checkpoint_threads != 0:
            (t_create, t_var) = self.parse_threads(thread_config,
                                                   checkpoint_threads)
            s += '\n' + t_create
            if reopen_connection:
                s += '\n# reopen the connection\n'
                s += 'conn.close()\n'
                if readonly:
                    'conn_config += ",readonly=true"\n'
                s += 'conn = context.wiredtiger_open("create," + conn_config)\n'
                s += '\n'
            s += 'workload = Workload(context, ' + t_var + ')\n'
            s += workloadopts

            if self.verbose > 0:
                s += 'print("workload:")\n'
            s += 'ret = workload.run(conn)\n'
            s += 'assert ret == 0, ret\n'
            s += 'latency_filename = context.args.home + "/latency.out"\n'
            s += 'latency.workload_latency(workload, latency_filename)\n'

        if close_conn:
            s += 'conn.close()\n'

        for o in self.opts_used:
            del self.opts_map[o]
        if len(self.opts_map) != 0:
            self.error('internal error, options not handled: ' +
                       str(self.opts_map))
        return s

def usage():
    eprint((
        'Usage: python wtperf.py [ options ] file.wtperf ...\n'
        '\n'
        'Options:\n'
        '    --python            Python output generated on stdout\n'
        ' -v --verbose           Verbose output\n'
        '\n'
        'If --python is not specified, the resulting workload is run.'))

verbose = 0
py_out = False
workgen_dir = os.path.dirname(os.path.abspath(__file__))
runner_dir = os.path.join(workgen_dir, 'runner')
prefix = (
  '# The next lines are unneeded if this script is in the runner directory.\n'
  'import sys\n'
  'sys.path.append("' + runner_dir + '")\n\n')

exit_status = 0
homedir = 'WT_TEST'
for arg in sys.argv[1:]:
    if arg == '--pydebug':
        import pdb
        pdb.set_trace()
    elif arg == '--python':
        py_out = True
    elif arg == '--verbose' or arg == '-v':
        verbose += 1
    elif arg.endswith('.wtperf'):
        translator = Translator(arg, prefix, verbose, homedir)
        pysrc = translator.translate()
        if translator.has_error:
            exit_status = 1
        elif py_out:
            print(pysrc)
        else:
            (outfd, tmpfile) = tempfile.mkstemp(suffix='.py')
            os.write(outfd, pysrc.encode())
            os.close(outfd)
            # We make a copy of the configuration file in the home
            # directory after the run, because the wiredtiger_open
            # in the generated code will clean out the directory first.
            raised = None
            ret = 0
            try:
                # Run python on the generated script
                ret = subprocess.call([sys.executable, tmpfile])
            except (KeyboardInterrupt, Exception) as exception:
                raised = exception
            if not os.path.isdir(homedir):
                os.makedirs(homedir)
            translator.copy_config()
            translator.copy_python_source(tmpfile)
            os.remove(tmpfile)
            if raised != None:
                raise raised
            if ret != 0:
                raise Exception('Running generated program returned ' +
                                str(ret))
    else:
        usage()
        sys.exit(1)
sys.exit(exit_status)
