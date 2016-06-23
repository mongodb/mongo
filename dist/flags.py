# Output a C header file using the minimum number of distinct bits to ensure
# flags don't collide.

import os, re, sys
from dist import compare_srcfile

flags = {
###################################################
# Internal routine flag declarations
###################################################
    'log_scan' : [
        'LOGSCAN_FIRST',
        'LOGSCAN_FROM_CKP',
        'LOGSCAN_ONE',
        'LOGSCAN_RECOVER',
    ],
    'log_write' : [
        'LOG_BACKGROUND',
        'LOG_DSYNC',
        'LOG_FLUSH',
        'LOG_FSYNC',
        'LOG_SYNC_ENABLED',
    ],
    'page_read' : [
        'READ_CACHE',
        'READ_COMPACT',
        'READ_NOTFOUND_OK',
        'READ_NO_EMPTY',
        'READ_NO_EVICT',
        'READ_NO_GEN',
        'READ_NO_WAIT',
        'READ_PREV',
        'READ_RESTART_OK',
        'READ_SKIP_INTL',
        'READ_SKIP_LEAF',
        'READ_TRUNCATE',
        'READ_WONT_NEED',
    ],
    'rec_write' : [
        'EVICT_IN_MEMORY',
        'EVICT_LOOKASIDE',
        'EVICT_UPDATE_RESTORE',
        'EVICTING',
        'VISIBILITY_ERR',
    ],
    'txn_log_checkpoint' : [
        'TXN_LOG_CKPT_CLEANUP',
        'TXN_LOG_CKPT_PREPARE',
        'TXN_LOG_CKPT_START',
        'TXN_LOG_CKPT_STOP',
        'TXN_LOG_CKPT_SYNC',
    ],
    'txn_update_oldest' : [
        'TXN_OLDEST_STRICT',
        'TXN_OLDEST_WAIT',
    ],
    'verbose' : [
        'VERB_API',
        'VERB_BLOCK',
        'VERB_CHECKPOINT',
        'VERB_COMPACT',
        'VERB_EVICT',
        'VERB_EVICTSERVER',
        'VERB_FILEOPS',
        'VERB_HANDLEOPS',
        'VERB_LOG',
        'VERB_LSM',
        'VERB_LSM_MANAGER',
        'VERB_METADATA',
        'VERB_MUTEX',
        'VERB_OVERFLOW',
        'VERB_READ',
        'VERB_REBALANCE',
        'VERB_RECONCILE',
        'VERB_RECOVERY',
        'VERB_SALVAGE',
        'VERB_SHARED_CACHE',
        'VERB_SPLIT',
        'VERB_TEMPORARY',
        'VERB_TRANSACTION',
        'VERB_VERIFY',
        'VERB_VERSION',
        'VERB_WRITE',
    ],

###################################################
# Structure flag declarations
###################################################
    'conn' : [
        'CONN_CACHE_POOL',
        'CONN_CKPT_SYNC',
        'CONN_CLOSING',
        'CONN_EVICTION_RUN',
        'CONN_IN_MEMORY',
        'CONN_LAS_OPEN',
        'CONN_LEAK_MEMORY',
        'CONN_LOG_SERVER_RUN',
        'CONN_LSM_MERGE',
        'CONN_PANIC',
        'CONN_READONLY',
        'CONN_RECOVERING',
        'CONN_SERVER_ASYNC',
        'CONN_SERVER_CHECKPOINT',
        'CONN_SERVER_LSM',
        'CONN_SERVER_RUN',
        'CONN_SERVER_STATISTICS',
        'CONN_SERVER_SWEEP',
        'CONN_WAS_BACKUP',
    ],
    'session' : [
        'SESSION_CAN_WAIT',
        'SESSION_INTERNAL',
        'SESSION_LOCK_NO_WAIT',
        'SESSION_LOCKED_CHECKPOINT',
        'SESSION_LOCKED_HANDLE_LIST',
        'SESSION_LOCKED_METADATA',
        'SESSION_LOCKED_PASS',
        'SESSION_LOCKED_SCHEMA',
        'SESSION_LOCKED_SLOT',
        'SESSION_LOCKED_TABLE',
        'SESSION_LOCKED_TURTLE',
        'SESSION_LOGGING_INMEM',
        'SESSION_LOOKASIDE_CURSOR',
        'SESSION_NO_CACHE',
        'SESSION_NO_DATA_HANDLES',
        'SESSION_NO_EVICTION',
        'SESSION_NO_LOGGING',
        'SESSION_NO_SCHEMA_LOCK',
        'SESSION_QUIET_CORRUPT_FILE',
        'SESSION_SERVER_ASYNC',
    ],
}

flag_cnt = {}    # Dictionary [flag] : [reference count]
flag_name = {}   # Dictionary [flag] : [name ...]
name_mask = {}   # Dictionary [name] : [used flag mask]

# Step through the flags dictionary and build our local dictionaries.
for method in flags.items():
    name_mask[method[0]] = 0x0
    for flag in method[1]:
        if flag == '__NONE__':
            continue
        if flag not in flag_cnt:
            flag_cnt[flag] = 1
            flag_name[flag] = []
        else:
            flag_cnt[flag] += 1
        flag_name[flag].append(method[0])

# Create list of possible bit masks.
bits = [2 ** i for i in range(0, 32)]

# Walk the list of flags in reverse, sorted-by-reference count order.  For
# each flag, find a bit that's not currently in use by any method using the
# flag.
flag_bit = {}    # Dictionary [flag] : [bit value]
for f in sorted(flag_cnt.items(), key = lambda k_v : (-k_v[1], k_v[0])):
    mask = 0xffffffff
    for m in flag_name[f[0]]:
        mask &= ~name_mask[m]
    if mask == 0:
        print >>sys.stderr,\
            "flags.py: ran out of flags at " + m + " method",
        sys.exit(1)
    for b in bits:
        if mask & b:
            mask = b
            break
    flag_bit[f[0]] = mask
    for m in flag_name[f[0]]:
        name_mask[m] |= mask

# Print out the flag masks in hex.
#    Assumes tab stops set to 8 characters.
flag_info = ''
for f in sorted(flag_cnt.items()):
    flag_info += "#define\tWT_%s%s%#010x\n" %\
        (f[0],\
        "\t" * max(1, 6 - int((len('WT_') + len(f[0])) / 8)),\
        flag_bit[f[0]])

# Update the wiredtiger.in file with the flags information.
tmp_file = '__tmp'
tfile = open(tmp_file, 'w')
skip = 0
for line in open('../src/include/flags.h', 'r'):
    if skip:
        if line.count('flags section: END'):
            tfile.write('/*\n' + line)
            skip = 0
    else:
        tfile.write(line)
    if line.count('flags section: BEGIN'):
        skip = 1
        tfile.write(' */\n')
        tfile.write(flag_info)
tfile.close()
compare_srcfile(tmp_file, '../src/include/flags.h')
