#!/usr/bin/env python
#
# Public Domain 2014-2017 MongoDB, Inc.
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
import os, sys, tempfile

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
    def __init__(self, filename, prefix, verbose):
        self.filename = filename
        self.prefix = prefix
        self.verbose = verbose
        self.linenum = 0
        self.opts = {}
        self.used_opts = {}
        self.has_error = False

    def error_file_line(self, fname, linenum, msg):
        self.has_error = True
        eprint(fname + ':' + str(linenum) + ': error: ' + msg)

    # Report an error and continue
    def error(self, msg):
        self.error_file_line(self.filename, self.linenum, msg)

    # Report an error and unwind the stack
    def fatal_error(self, msg, errtype):
        self.error(msg)
        raise TranslateException(errtype)

    supported_opt_list = [ 'compression', 'conn_config', 'icount',
                           'key_sz', 'log_like_table',
                           'populate_ops_per_txn', 'populate_threads',
                           'reopen_connection',
                           'table_config', 'table_count',
                           'threads', 'transaction_config', 'value_sz' ]

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
        self.opts[optname] = OptionValue(v, self.filename, self.linenum)

    def get_opt(self, optname, dfault):
        if optname in self.opts:
            ret = self.opts[optname]
            self.filename = ret.filename
            self.linenum = ret.linenum
            self.used_opts[optname] = 1
            return ret.value
        else:
            return dfault

    def get_int_opt(self, optname, dfault):
        return self.get_opt(optname, dfault) + 0

    def get_boolean_opt(self, optname, dfault):
        return not not self.get_opt(optname, dfault)

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
            import pdb
            pdb.set_trace()
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

    def add_operation_str(self, count, opname, multi):
        result = ''
        tablename = 'tables[0]' if multi else 'table'
        if count > 1:
            result += str(count) + ' * '
        if count > 0:
            result += 'Operation(Operation.' + opname + ', ' + \
                      tablename + ') + \\\n'
            result += '      '
        return result

    # Wtperf's throttle is based on the number of regular operations,
    # not including log_like operations.  Workgen counts all operations,
    # it doesn't treat log operations any differently.  Adjust the throttle
    # number to account for the difference.
    def calc_throttle(self, thread_opts, log_like_table):
        throttle = thread_opts.throttle
        if not log_like_table:
            return (throttle, '')
        modify = thread_opts.inserts + thread_opts.updates
        regular = modify + thread_opts.reads
        total = regular + modify
        factor = (total + 0.0) / regular
        new_throttle = int(throttle * factor)
        if new_throttle == throttle:
            comment = ''
        else:
            comment = '# wtperf throttle=' + str(throttle) + ' adjusted by ' + \
                      str(factor) + ' to compensate for log_like operations.\n'
        return (new_throttle, comment)

    def parse_threads(self, threads_config):
        tdecls = ''
        tlist = self.split_config_parens(threads_config)
        table_count = self.get_int_opt('table_count', 1)
        log_like_table = self.get_boolean_opt('log_like_table', False)
        txn_config = self.get_opt('transaction_config', '')
        if log_like_table:
            tdecls += 'log_name = "table:log"\n'
            tdecls += 's.create(log_name, "key_format=S,value_format=S," +' + \
                      ' compress_table_config)\n'
            tdecls += 'log_table = Table(log_name)\n\n'
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
            topts.update = 0
            topts.updates = 0

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
            tdecls += self.add_operation_str(topts.inserts, 'OP_INSERT', multi)
            tdecls += self.add_operation_str(topts.reads, 'OP_SEARCH', multi)
            tdecls += self.add_operation_str(topts.updates, 'OP_UPDATE', multi)
            tdecls = tdecls.rstrip(' \n\\+') + '\n'
            if multi:
                tdecls += 'ops = op_multi_table(ops, tables)\n'
            if topts.ops_per_txn > 0:
                tdecls += 'ops = op_group_transaction(ops, ' + \
                          str(topts.ops_per_txn) + ', "' + txn_config + '")\n'
            if log_like_table:
                tdecls += 'ops = op_log_like(ops, log_table, ' + \
                          str(topts.ops_per_txn) + ')\n'
            tdecls += thread_name + ' = Thread(ops)\n'
            if topts.throttle > 0:
                (throttle, comment) = self.calc_throttle(topts, log_like_table)
                tdecls += comment
                tdecls += self.assign_str(thread_name + '.options.throttle',
                                          throttle)
            tdecls += '\n'
            if topts.count > 1:
                tnames += str(topts.count) + ' * '
            tnames += thread_name + ' + '

        tnames = tnames.rstrip(' +')
        return (tdecls, tnames)

    def translate(self):
        try:
            return self.translate_inner()
        except TranslateException:
            # An error has already been reported
            return None

    def translate_inner(self):
        workloadopts = ''
        with open(self.filename) as fin:
            for line in fin:
                self.linenum += 1
                commentpos = line.find('#')
                if commentpos >= 0:
                    line = line[0:commentpos]
                line = line.strip()
                if len(line) == 0:
                    continue
                (key, val) = self.split_assign(line)
                if key in [ 'max_latency', 'report_file', 'report_interval',
                            'run_time', 'sample_interval', 'sample_rate' ]:
                    workloadopts += 'workload.options.' + key + '=' + val + '\n'
                else:
                    self.set_opt(key, val)

        table_count = self.get_int_opt('table_count', 1)
        conn_config = self.get_opt('conn_config', '')
        table_config = self.get_opt('table_config', '')
        key_sz = self.get_int_opt('key_sz', 20)
        value_sz = self.get_int_opt('value_sz', 100)
        reopen = self.get_boolean_opt('reopen_connection', False)
        compression = self.get_opt('compression', '')
        txn_config = self.get_opt('transaction_config', '')

        s = '#/usr/bin/env python\n'
        s += '# generated from ' + self.filename + '\n'
        s += self.prefix
        s += 'from runner import *\n'
        s += 'from wiredtiger import *\n'
        s += 'from workgen import *\n'
        s += '\n'
        s += 'context = Context()\n'
        s += 'conn_config = "' + conn_config + '"\n'
        if compression != '':
            s += 'conn_config += extensions_config(["compressors/' + \
                 compression + '"])\n'
            compression = 'block_compressor=' + compression + ','
        s += 'conn = wiredtiger_open("WT_TEST", "create," + conn_config)\n'
        s += 's = conn.open_session()\n'
        s += '\n'
        s += 'wtperf_table_config = "key_format=S,value_format=S,type=lsm," +\\\n'
        s += '    "exclusive=true,allocation_size=4kb," +\\\n'
        s += '    "internal_page_max=64kb,leaf_page_max=4kb,split_pct=100,"\n'
        s += 'compress_table_config = "' + compression + '"\n'
        s += 'table_config = "' + table_config + '"\n'
        if table_count == 1:
            s += 'tname = "file:test.wt"\n'
            s += 's.create(tname, wtperf_table_config +\\\n'
            s += '         compress_table_config + table_config)\n'
            s += 'table = Table(tname)\n'
            s += 'table.options.key_size = ' + str(key_sz) + '\n'
            s += 'table.options.value_size = ' + str(value_sz) + '\n'
        else:
            s += 'table_count = ' + str(table_count) + '\n'
            s += 'tables = []\n'
            s += 'for i in range(0, table_count):\n'
            s += '    tname = "file:test" + str(i) + ".wt"\n'
            s += '    s.create(tname, ' + \
                 'wtperf_table_config + ' + \
                 'compress_table_config + table_config)\n'
            s += '    t = Table(tname)\n'
            s += '    t.options.key_size = ' + str(key_sz) + '\n'
            s += '    t.options.value_size = ' + str(value_sz) + '\n'
            s += '    tables.append(t)\n'
            s += '\n'

        icount = self.get_int_opt('icount', 0)
        pop_thread = self.get_int_opt('populate_threads', 1)
        pop_per_txn = self.get_int_opt('populate_ops_per_txn', 0)
        if icount != 0:
            if pop_thread == 0:
                self.fatal_error('icount != 0 and populate_threads == 0: ' +\
                                 'cannot populate entries with no threads')
            elif pop_thread == 1:
                mult = ''
            else:
                mult = str(pop_thread) + ' * '

            # if there are multiple tables to be filled during populate,
            # the icount is split between them all.
            nops_per_thread = icount / (pop_thread * table_count)
            if table_count == 1:
                s += 'pop_ops = Operation(Operation.OP_INSERT, table)\n'
            else:
                s += 'pop_ops = Operation(Operation.OP_INSERT, tables[0])\n'
                s += 'pop_ops = op_multi_table(pop_ops, tables)\n'
            if pop_per_txn > 0:
                s += 'pop_ops = op_group_transaction(pop_ops, ' + \
                          str(pop_per_txn) + ', "' + txn_config + '")\n'
            s += 'pop_thread = Thread(pop_ops * ' + str(nops_per_thread) + ')\n'
            s += 'pop_workload = Workload(context, ' + mult + 'pop_thread)\n'
            if self.verbose > 0:
                s += 'print("populate:")\n'
            s += 'pop_workload.run(conn)\n'
        else:
            if self.get_int_opt('populate_threads', 0) != 0:
                self.error("populate_threads > 0, icount == 0")

        thread_config = self.get_opt('threads', '')
        if thread_config != '':
            (t_create, t_var) = self.parse_threads(thread_config)
            s += '\n' + t_create
            if reopen:
                s += '\n# reopen the connection\n'
                s += 'conn.close()\n'
                s += 'conn = wiredtiger_open(' + \
                     '"WT_TEST", "create," + conn_config)\n'
                s += '\n'
            s += 'workload = Workload(context, ' + t_var + ')\n'
            s += workloadopts
            if self.verbose > 0:
                s += 'print("workload:")\n'
            s += 'workload.run(conn)\n'

        for o in self.used_opts:
            del self.opts[o]
        if len(self.opts) != 0:
            self.error('internal error, options not handled: ' + str(self.opts))
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
for arg in sys.argv[1:]:
    if arg == '--python':
        py_out = True
    elif arg == '--verbose' or arg == '-v':
        verbose += 1
    elif arg.endswith('.wtperf'):
        translator = Translator(arg, prefix, verbose)
        pysrc = translator.translate()
        if translator.has_error:
            exit_status = 1
        elif py_out:
            print(pysrc)
        else:
            (outfd, tmpfile) = tempfile.mkstemp(suffix='.py')
            os.write(outfd, pysrc)
            os.close(outfd)
            execfile(tmpfile)
            os.remove(tmpfile)
    else:
        usage()
        sys.exit(1)
sys.exit(exit_status)
