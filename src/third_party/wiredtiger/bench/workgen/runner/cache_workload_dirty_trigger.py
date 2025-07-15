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

# Generated from runner/read_write_heavy.wtperf originally, then hand edited.
# Create waves of extra read and write activity (storms).

from runner import *
from wiredtiger import *
from workgen import *

def get_cache_eviction_stats(session, cache_eviction_file):

    if cache_eviction_file:
        fh = open(cache_eviction_file, 'a')
    else:
        fh = sys.stdout

    stat_cursor = session.open_cursor('statistics:')
    print('----- Start of Cache Eviction statistics -----', file=fh)
    # Cache statistics
    cache_total = stat_cursor[wiredtiger.stat.conn.cache_bytes_max][2]
    print('Cache size          : 100 % :' + str(cache_total), file=fh)
    bytes_inuse = stat_cursor[wiredtiger.stat.conn.cache_bytes_inuse][2]
    value = ((bytes_inuse / cache_total) * 100 )
    print('Cache_bytes_inuse   : ' + str(round(value,2)) +' % : ' + str(bytes_inuse), file=fh)
    bytes_image = stat_cursor[wiredtiger.stat.conn.cache_bytes_image][2]
    value = ((bytes_image / cache_total) * 100 )
    print('Cache_bytes_image   : ' + str(round(value,2)) +' % : ' + str(bytes_image), file=fh)
    bytes_updates = stat_cursor[wiredtiger.stat.conn.cache_bytes_updates][2]
    value = ((bytes_updates / cache_total) * 100 )
    print('Cache_bytes_updates : ' + str(round(value,2)) +' % : ' + str(bytes_updates), file=fh)
    bytes_dirty = stat_cursor[wiredtiger.stat.conn.cache_bytes_dirty][2]
    value = ((bytes_dirty / cache_total) * 100 )
    print('Cache_bytes_dirty   : ' + str(round(value,2)) +' % : ' + str(bytes_dirty), file=fh)


    # History store statistics
    bytes_hs = stat_cursor[wiredtiger.stat.conn.cache_bytes_hs][2]
    value = ((bytes_hs / cache_total) * 100 )
    print('Cache_bytes_hs      : ' + str(round(value,2)) +' % : ' + str(bytes_hs), file=fh)
    bytes_hs_dirty = stat_cursor[wiredtiger.stat.conn.cache_bytes_hs_dirty][2]
    value = ((bytes_hs_dirty / cache_total) * 100 )
    print('Cache_bytes_hs_dirty : ' + str(round(value,2)) +' % : ' + str(bytes_hs_dirty), file=fh)
    bytes_hs_updates = stat_cursor[wiredtiger.stat.conn.cache_bytes_hs_updates][2]
    value = ((bytes_hs_updates / cache_total) * 100 )
    print('Cache_bytes_hs_updates : ' + str(round(value,2)) +' % : ' + str(bytes_hs_updates), file=fh)
    print(' ', file=fh)

    # Cache configured trigger statistics
    trigger_updates = stat_cursor[wiredtiger.stat.conn.cache_eviction_trigger_updates_reached][2]
    print('Cache updates trigger  : ' + str(trigger_updates), file=fh)
    trigger_dirty = stat_cursor[wiredtiger.stat.conn.cache_eviction_trigger_dirty_reached][2]
    print('Cache dirty trigger : ' + str(trigger_dirty), file=fh)
    trigger_usage = stat_cursor[wiredtiger.stat.conn.cache_eviction_trigger_reached][2]
    print('Cache usage trigger : ' + str(trigger_usage), file=fh)
    print(' ', file=fh)

    # App cache statistics
    value = stat_cursor[wiredtiger.stat.conn.cache_read_app_count][2]
    print('App pages read  : ' + str(value), file=fh)
    value = stat_cursor[wiredtiger.stat.conn.cache_write_app_count][2]
    print('App pages wrote : ' + str(value), file=fh)
    print(' ', file=fh)

    #App eviction statistics
    app_dirty_attempt = stat_cursor[wiredtiger.stat.conn.eviction_app_dirty_attempt][2]
    print('App evict dirty attempts        : ' + str(app_dirty_attempt), file=fh)
    app_dirty_fail = stat_cursor[wiredtiger.stat.conn.eviction_app_dirty_fail][2]
    print('App evict dirty attempts failed : ' + str(app_dirty_fail), file=fh)
    app_attempt = stat_cursor[wiredtiger.stat.conn.eviction_app_attempt][2]
    print('App evict clean attempts        : ' + str(app_attempt - app_dirty_attempt), file=fh)
    app_fail = stat_cursor[wiredtiger.stat.conn.eviction_app_fail][2]
    print('App evict clean attempts failed : ' + str(app_fail - app_dirty_fail), file=fh)
    print(' ', file=fh)

    # Force eviction statistics
    force = stat_cursor[wiredtiger.stat.conn.eviction_force][2]
    print('Force evict attempts         : ' + str(force), file=fh)
    force_clean = stat_cursor[wiredtiger.stat.conn.eviction_force_clean][2]
    print('Force evict clean attempts   : ' + str(force_clean), file=fh)
    force_dirty = stat_cursor[wiredtiger.stat.conn.eviction_force_dirty][2]
    print('Force evict dirty attempts   : ' + str(force_dirty), file=fh)
    force_long_update = stat_cursor[wiredtiger.stat.conn.eviction_force_long_update_list][2]
    print('Force evict long update list : ' + str(force_long_update), file=fh)
    force_delete = stat_cursor[wiredtiger.stat.conn.eviction_force_delete][2]
    print('Force evict too many deletes : ' + str(force_delete), file=fh)
    force_fail = stat_cursor[wiredtiger.stat.conn.eviction_force_fail][2]
    print('Force evict failed           : ' + str(force_fail), file=fh)
    print(' ', file=fh)

    # Eviction worker statistics
    worker_attempt = stat_cursor[wiredtiger.stat.conn.eviction_worker_evict_attempt][2]
    print('Eviction worker attempts        : ' + str(worker_attempt), file=fh)
    worker_attempt_fail = stat_cursor[wiredtiger.stat.conn.eviction_worker_evict_fail][2]
    print('Eviction worker attempts failed : ' + str(worker_attempt_fail), file=fh)
    print('----- End of Cache Eviction statistics -----', file=fh)
    print(' ', file=fh)


    stat_cursor.close()

context = Context()
# eviction_updates_trigger=30
conn_config = ""
conn_config += ",cache_size=10GB,eviction=(threads_max=8),log=(enabled=true),session_max=250,statistics=(fast),statistics_log=(wait=1,json=true),io_capacity=(total=30M)"   # explicitly added
conn = context.wiredtiger_open("create," + conn_config)
s = conn.open_session("")

wtperf_table_config = "key_format=S,value_format=S," +\
    "exclusive=true,allocation_size=4kb," +\
    "internal_page_max=64kb,leaf_page_max=4kb,split_pct=100,"
compress_table_config = ""
table_config = "memory_page_max=10m,leaf_value_max=64MB,checksum=on,split_pct=90,type=file,log=(enabled=false),leaf_page_max=32k"
tables = []
table_count = 10
# Configure key and value sizes.
cfg_key_size = 10
cfg_value_size = 2000
for i in range(0, table_count):
    tname = "table:test" + str(i)
    table = Table(tname)
    s.create(tname, wtperf_table_config +\
             compress_table_config + table_config)
    table.options.key_size = cfg_key_size
    table.options.value_size = cfg_value_size
    tables.append(table)

populate_threads = 4
icount = 1000000
# There are multiple tables to be filled during populate,
# the icount is split between them all.
pop_ops = Operation(Operation.OP_INSERT, tables[0])
pop_ops = op_multi_table(pop_ops, tables)
nops_per_thread = icount // (populate_threads * table_count)
pop_thread = Thread(pop_ops * nops_per_thread)
pop_workload = Workload(context, populate_threads * pop_thread)
ret = pop_workload.run(conn)
assert ret == 0, ret
cache_eviction_file = context.args.home + "/cache_eviction.stat"
get_cache_eviction_stats(s, cache_eviction_file)

print('Populate complete')

# Log like file, requires that logging be enabled in the connection config.
log_name = "table:log"
s.create(log_name, wtperf_table_config + "key_format=S,value_format=S," + compress_table_config + table_config + ",log=(enabled=true)")
log_table = Table(log_name)
log_table.options.key_size = cfg_key_size
log_table.options.value_size = cfg_value_size

ops = Operation(Operation.OP_UPDATE, log_table)
ops = op_log_like(ops, log_table, 0)
thread_log_upd = Thread(ops)
thread_log_upd.options.session_config="isolation=snapshot"
# These operations include log_like operations, which will increase the number
# of insert/update operations by a factor of 2.0. This may cause the
# actual operations performed to be above the throttle.
thread_log_upd.options.throttle=11
thread_log_upd.options.throttle_burst=0

ops = Operation(Operation.OP_SEARCH, log_table)
ops = op_log_like(ops, log_table, 0)
thread_log_read = Thread(ops)
thread_log_read.options.session_config="isolation=snapshot"
thread_log_read.options.throttle=60
thread_log_read.options.throttle_burst=0

############################################################################
# This part was added to the generated file.
# Add threads that do a bunch of operations and sleep, all in a loop.
# At the beginning of the run the threads will tend to be synchronized,
# but that effect will dissipate over time.

# This workload is to hit the update trigger
ops = Operation(Operation.OP_UPDATE, tables[0])
ops = txn(ops, 'isolation=snapshot')
# use_commit_timestamp - Commit the transaction with commit_timestamp.
ops.transaction.use_commit_timestamp = True
ops = op_multi_table(ops, tables, False)
ops = ops * 10000 + Operation(Operation.OP_SLEEP, "10")
thread_upd10k_sleep10 = Thread(ops)
thread_upd10k_sleep10.options.name = "Update"
thread_upd10k_sleep10.options.session_config="isolation=snapshot"

ops = Operation(Operation.OP_SEARCH, tables[0])
ops = txn(ops, 'isolation=snapshot')
# use_commit_timestamp - Commit the transaction with commit_timestamp.
ops.transaction.use_commit_timestamp = True
ops = op_multi_table(ops, tables, False)
ops = ops * 10000 + Operation(Operation.OP_SLEEP, "10")
thread_read10k_sleep10 = Thread(ops)
thread_upd10k_sleep10.options.name = "Search"
thread_read10k_sleep10.options.session_config="isolation=snapshot"

# End of added section.
# The new threads will also be added to the workload below.
############################################################################

#Configure only the expected % of read operations
#15% update workload to hit the cache dirty trigger threshold
read_ops = 85

#Specify number of threads for workload
total_thread_num = 128
log_read_thread_num = 1
log_write_thread_num = 1
#Calculate threads for read operations based on % of read operations
read_thread_num = int(((read_ops * total_thread_num) / 100))
#Remaining threads for write operations
write_thread_num = (total_thread_num - read_thread_num)

cache_workload = Workload(context,\
                        + log_write_thread_num * thread_log_upd \
                        + log_read_thread_num * thread_log_read \
                        + write_thread_num * thread_upd10k_sleep10 \
                        + read_thread_num * thread_read10k_sleep10)

cache_workload.options.report_interval=10
# max operational latency in milli seconds.
cache_workload.options.max_latency=1000
cache_workload.options.run_time=200
cache_workload.options.sample_rate=1
cache_workload.options.warmup=0
cache_workload.options.sample_interval_ms = 1000

ret = cache_workload.run(conn)
assert ret == 0, ret

get_cache_eviction_stats(s, cache_eviction_file)

latency_filename = context.args.home + "/latency.stat"
latency.workload_latency(cache_workload, latency_filename)
