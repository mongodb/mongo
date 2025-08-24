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

# cache.py
# Print cachemetrics for workgen
import json, sys
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
