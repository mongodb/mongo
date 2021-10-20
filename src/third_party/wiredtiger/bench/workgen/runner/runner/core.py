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
# runner/core.py
#   Core functions available to all runners
import glob, os, random
from workgen import Key, Operation, OpList, Table, Transaction, Value

# txn --
#   Put the operation (and any suboperations) within a transaction.
def txn(op, config=None):
    t = Transaction(config)
    op._transaction = t
    return op

# Check for a local build that contains the wt utility. First check in
# current working directory, then in build_posix and finally in the disttop
# directory. This isn't ideal - if a user has multiple builds in a tree we
# could pick the wrong one.
def _wiredtiger_builddir():
    if os.path.isfile(os.path.join(os.getcwd(), 'wt')):
        return os.getcwd()

    # The directory of this file should be within the distribution tree.
    thisdir = os.path.dirname(os.path.abspath(__file__))
    wt_disttop = os.path.join(\
        thisdir, os.pardir, os.pardir, os.pardir, os.pardir)
    if os.path.isfile(os.path.join(wt_disttop, 'wt')):
        return wt_disttop
    if os.path.isfile(os.path.join(wt_disttop, 'build_posix', 'wt')):
        return os.path.join(wt_disttop, 'build_posix')
    if os.path.isfile(os.path.join(wt_disttop, 'wt.exe')):
        return wt_disttop
    raise Exception('Unable to find useable WiredTiger build')

# Return the wiredtiger_open extension argument for any needed shared library.
# Called with a list of extensions, e.g.
#    [ 'compressors/snappy', 'encryptors/rotn=config_string' ]
def extensions_config(exts):
    result = ''
    extfiles = {}
    errpfx = 'extensions_config'
    builddir = _wiredtiger_builddir()
    for ext in exts:
        extconf = ''
        if '=' in ext:
            splits = ext.split('=', 1)
            ext = splits[0]
            extconf = '=' + splits[1]
        splits = ext.split('/')
        if len(splits) != 2:
            raise Exception(errpfx + ": " + ext +
                ": extension is not named <dir>/<name>")
        libname = splits[1]
        dirname = splits[0]
        pat = os.path.join(builddir, 'ext',
            dirname, libname, '.libs', 'libwiredtiger_*.so')
        filenames = glob.glob(pat)
        if len(filenames) == 0:
            raise Exception(errpfx +
                ": " + ext +
                ": no extensions library found matching: " + pat)
        elif len(filenames) > 1:
            raise Exception(errpfx + ": " + ext +
                ": multiple extensions libraries found matching: " + pat)
        complete = '"' + filenames[0] + '"' + extconf
        if ext in extfiles:
            if extfiles[ext] != complete:
                raise Exception(errpfx +
                    ": non-matching extension arguments in " +
                    str(exts))
        else:
            extfiles[ext] = complete
    if len(extfiles) != 0:
        result = ',extensions=[' + ','.join(extfiles.values()) + ']'
    return result

_PARETO_SHAPE = 1.5
_BILLION = 1000000000

# Choose a value from a range of ints based on the pareto parameter
# The pareto value is interpreted as in wtperf, a number between 0 and 100.
def _choose_pareto(nrange, pareto):
    rval = random.randint(0, _BILLION)

    # Use Pareto distribution to give 80/20 hot/cold values.
    S1 = -1 / _PARETO_SHAPE
    S2 = nrange * (pareto.param / 100.0) * (_PARETO_SHAPE - 1)
    U = 1 - rval / (_BILLION * 1.0)
    rval = (pow(U, S1) - 1) * S2
    if rval >= nrange:
        rval = 0
    return int(rval)

# Get the list of subordinate operations that are listed in the group.
# Generally, the op._optype == Operation.OP_NONE, it indicates that
# the operation contains a group of subordinates.
#
# XXX
# Note that this function should be called for all iteration, rather than:
#    for o in op._group
# because a bug in SWIG versions <= 2.0.11 would cause the above fragment
# to produce a segmentation violation as described here:
#    https://sourceforge.net/p/swig/mailman/message/32838320/
def _op_get_group_list(op):
    grouplist = op._group
    result = []
    if grouplist != None:
        result.extend(grouplist)
    return result

def _op_multi_table_as_list(ops_arg, tables, pareto_tables, multiplier):
    result = []
    if ops_arg._optype != Operation.OP_NONE:
        if pareto_tables <= 0:
            for table in tables:
                for i in range(0, multiplier):
                    result.append(Operation(ops_arg._optype, table, ops_arg._key, ops_arg._value))
        else:
            # Use the multiplier unless the length of the list will be large.
            # In any case, make sure there's at least a multiplier of 3, to
            # give a chance to hit all/most of the tables.
            ntables = len(tables)
            count = ntables * multiplier
            if count > 1000:
                count = 1000
                mincount = ntables * 3
                if mincount > count:
                    count = mincount
            for i in range(0, count):
                tnum = _choose_pareto(ntables, pareto_tables)
                # Modify the pareto value to make it more flat
                # as tnum gets higher.  Workgen knows how to handle
                # a portion of a pareto range.
                table = tables[tnum]
                key = Key(ops_arg._key)
                key._pareto.range_low = (1.0 * i)/count
                key._pareto.range_high = (1.0 * (i + 1))/count
                result.append(Operation(ops_arg._optype, table, key, ops_arg._value))
    else:
        for op in _op_get_group_list(ops_arg):
            for o in _op_multi_table_as_list(op, tables, pareto_tables, \
                                             multiplier):
                result.append(Operation(o))
    return result

# A convenient way to build a list of operations
def op_append(op1, op2):
    if op1 == None:
        op1 = op2
    else:
        op1 += op2
    return op1

# Require consistent use of pareto on the set of operations,
# that keeps our algorithm reasonably simple.
def _check_pareto(ops_arg, cur = 0):
    if ops_arg._key != None and ops_arg._key._keytype == Key.KEYGEN_PARETO:
        p = ops_arg._key._pareto
        if cur != 0 and p != cur:
            raise Exception('mixed pareto values for ops within a ' + \
                            'single thread not supported')
        cur = p
    if ops_arg._group != None:
        for op in _op_get_group_list(ops_arg):
            cur = _check_pareto(op, cur)
    return cur

_primes = [83, 89, 97, 101, 103, 107, 109, 113]

# Emulate wtperf's table_count option.  Spread the given operations over
# a set of tables.  For example, given 5 operations and 4 tables, we return
# a set of 20 operations for all possibilities.
#
# When we detect that pareto is used with a range partition, things get
# trickier, because we'll want a higher proportion of operations channelled
# to the first tables.  Workgen only supports individual operations on a
# single table, so to get good Pareto distribution, we first expand the
# number in the total set of operations, and then choose a higher proportion
# of the tables.  We need to expand the number of operations to make sure
# that the lower tables get some hits.  While it's not perfect (without
# creating a huge multiplier) it's a reasonable approximation for most
# cases.  Within each table's access, the pareto parameters have to be
# adjusted to account for the each table's position in the total
# distribution.  For example, the lowest priority table will have a much
# more even distribution.
def op_multi_table(ops_arg, tables, range_partition = False):
    ops = None
    multiplier = 1
    if range_partition:
        pareto_tables = _check_pareto(ops_arg)
    else:
        pareto_tables = 0
    if pareto_tables != 0:
        multiplier = _primes[random.randint(0, len(_primes) - 1)]
    ops_list = _op_multi_table_as_list(ops_arg, tables, pareto_tables, \
                                       multiplier)
    if pareto_tables != 0:
        random.shuffle(ops_list)
    for op in ops_list:
        ops = op_append(ops, op)
    return ops

# should be 8 bytes format 'Q'
_logkey = Key(Key.KEYGEN_APPEND, 8)
def _op_log_op(op, log_table):
    keysize = op._key._size
    if keysize == 0:
        keysize = op._table.options.key_size
    valuesize = op._value._size
    if valuesize == 0:
        valuesize = op._table.options.value_size
    v = Value(keysize + valuesize)
    return Operation(Operation.OP_INSERT, log_table, _logkey, v)

def _optype_is_write(optype):
    return optype == Operation.OP_INSERT or optype == Operation.OP_UPDATE or \
        optype == Operation.OP_REMOVE

# Emulate wtperf's log_like option.  For all operations, add a second
# insert operation going to a log table.
def op_log_like(op, log_table, ops_per_txn):
    if op._optype != Operation.OP_NONE:
        if _optype_is_write(op._optype):
            op += _op_log_op(op, log_table)
            if ops_per_txn == 0:
                op = txn(op)       # txn for each action.
    else:
        oplist = []
        for op2 in _op_get_group_list(op):
            if op2._optype == Operation.OP_NONE:
                oplist.append(op_log_like(op2, log_table))
            elif ops_per_txn == 0 and _optype_is_write(op2._optype):
                op2 += _op_log_op(op2, log_table)
                oplist.append(txn(op2))       # txn for each action.
            else:
                oplist.append(op2)
                if _optype_is_write(op2._optype):
                    oplist.append(_op_log_op(op2, log_table))
        op._group = OpList(oplist)
    return op

def _op_transaction_list(oplist, txn_config):
    result = None
    for op in oplist:
        result = op_append(result, op)
    return txn(result, txn_config)

# Emulate wtperf's ops_per_txn option.  Create transactions around
# groups of operations of the indicated size.
def op_group_transaction(ops_arg, ops_per_txn, txn_config):
    if ops_arg != Operation.OP_NONE:
        return txn(ops_arg, txn_config)
    if ops_arg._transaction != None:
        raise Exception('nested transactions not supported')
    if ops_arg._repeatgroup != None:
        raise Exception('grouping transactions with multipliers not supported')

    oplist = []
    txgroup = []
    for op in _op_get_group_list(ops_arg):
        if op.optype == Operation.OP_NONE:
            oplist.append(_op_transaction_list(txgroup, txn_config))
            txgroup = []
            oplist.append(op)
        else:
            txgroup.append(op)
            if len(txgroup) >= ops_per_txn:
                oplist.append(_op_transaction_list(txgroup, txn_config))
                txgroup = []
    if len(txgroup) > 0:
        oplist.append(_op_transaction_list(txgroup, txn_config))
    ops_arg._group = OpList(oplist)
    return ops_arg

# Populate using range partition with the random range.
# We will totally fill 0 or more tables (fill_tables), and 0 or
# 1 table will be partially filled.  The rest (if any) will
# by completely unfilled, to be filled/accessed during
# the regular part of the run.
def op_populate_with_range(ops_arg, tables, icount, random_range, pop_threads):
    table_count = len(tables)
    entries_per_table = (icount + random_range) / table_count
    if entries_per_table == 0:
        # This can happen if table_count is huge relative to
        # icount/random_range.  Not really worth handling.
        raise Exception('table_count > (icount + random_range), seems absurd')
    if (icount + random_range) % table_count != 0:
        # This situation is not handled well by our simple algorithm,
        # we won't get exactly icount entries added during the populate.
        raise Exception('(icount + random_range) is not evenly divisible by ' +
                        'table_count')
    if entries_per_table % pop_threads != 0:
        # Another situation that is not handled exactly.
        raise Exception('(icount + random_range) is not evenly divisible by ' +
                        'populate_threads')
    fill_tables = icount / entries_per_table
    fill_per_thread = entries_per_table / pop_threads
    ops = None
    for i in range(0, fill_tables):
        op = Operation(ops_arg)
        op._table = tables[i]
        ops = op_append(ops, op * fill_per_thread)
    partial_fill = icount % entries_per_table
    if partial_fill > 0:
        fill_per_thread = partial_fill / pop_threads
        op = Operation(ops_arg)
        op._table = tables[fill_tables]
        ops = op_append(ops, op * fill_per_thread)
    return ops
