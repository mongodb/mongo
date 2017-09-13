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
# runner/core.py
#   Core functions available to all runners
import glob, os
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

def _op_multi_table_as_list(ops_arg, tables):
    result = []
    if ops_arg._optype != Operation.OP_NONE:
        for table in tables:
            result.append(Operation(ops_arg._optype, table, ops_arg._key, ops_arg._value))
    else:
        for op in ops._group:
            result.extend(_op_multi_table_as_list(op, tables))
    return result

# A convenient way to build a list of operations
def op_append(op1, op2):
    if op1 == None:
        op1 = op2
    else:
        op1 += op2
    return op1

# Emulate wtperf's table_count option.  Spread the given operations over
# a set of tables.
def op_multi_table(ops_arg, tables):
    ops = None
    for op in _op_multi_table_as_list(ops_arg, tables):
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
        for op2 in op._group:
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
    ops = None
    nops = 0
    txgroup = []
    for op in ops_arg._group:
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
