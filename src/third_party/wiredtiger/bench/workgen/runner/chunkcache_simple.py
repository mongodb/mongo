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
# Run a simple workload on the chunk cache using insert and read. First insert data onto the table,
# reopen the connection and configure the chunk cache. Perform reads to update the chunk cache.

from runner import *
from wiredtiger import *
from wiredtiger import stat
from workgen import *
import os

def get_stat(session, stat):
    stat_cursor = session.open_cursor('statistics:')
    val = stat_cursor[stat][2]
    stat_cursor.close()
    return val

def tiered_config(home):
    bucket_path = home + "/" + "bucket2"
    if not os.path.isdir(bucket_path):
        os.mkdir(bucket_path)

# Set up the WiredTiger connection.
context = Context()
wt_builddir = os.getenv('WT_BUILDDIR')
if not wt_builddir:
    wt_builddir = os.getcwd()
conn_config = f'create,statistics=(all),statistics_log=(wait=1,json=true,on_close=true),\
    tiered_storage=(auth_token=Secret,bucket=bucket2,bucket_prefix=pfx_,name=dir_store),\
        extensions=({wt_builddir}/ext/storage_sources/dir_store/libwiredtiger_dir_store.so=(early_load=true))'
chunkcache_config =  conn_config + f',chunk_cache=[enabled=true,chunk_size=10MB,capacity=1GB,type=FILE,storage_path=WiredTigerChunkCache]'
conn = context.wiredtiger_open(conn_config, tiered_config)
s = conn.open_session()
tname = 'table:chunkcache'
s.create(tname, 'key_format=S,value_format=S')
table = Table(tname)
table.options.key_size = 20
table.options.value_size = 100

# Populate phase.
insert_ops = Operation(Operation.OP_INSERT, table)
insert_thread = Thread(insert_ops * 100000)
populate_workload = Workload(context, insert_thread * 40)
ret = populate_workload.run(conn)
s.checkpoint()
s.checkpoint('flush_tier=(enabled)')
assert ret == 0, ret

# Reopen the connection and reconfigure.
conn.close()
conn = context.wiredtiger_open(chunkcache_config)
s = conn.open_session()

# Read into the chunk cache.
read_op = Operation(Operation.OP_SEARCH, table)
read_thread = Thread(read_op * 100000)
read_workload = Workload(context, read_thread * 40)
read_workload.options.run_time = 20
read_workload.options.report_interval = 1
ret = read_workload.run(conn)
assert ret == 0, ret

# Check relevant stats.
assert get_stat(s, wiredtiger.stat.conn.chunkcache_chunks_inuse) > 0

# Close the connection.
conn.close()
