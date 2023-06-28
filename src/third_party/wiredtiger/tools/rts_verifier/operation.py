#!/usr/bin/env python3

import os, re

from basic_types import PrepareState, Timestamp, UpdateType
from enum import Enum

class OpType(Enum):
    INIT = 0
    TREE = 1
    TREE_LOGGING = 2
    PAGE_ROLLBACK = 3
    UPDATE_ABORT = 4
    PAGE_ABORT_CHECK = 5
    KEY_CLEAR_REMOVE = 6
    ONDISK_KV_REMOVE = 7
    SHUTDOWN_INIT = 8
    TREE_SKIP = 9
    SKIP_DEL_NULL = 10
    ONDISK_ABORT_TW = 11
    ONDISK_KEY_ROLLBACK = 12
    HS_UPDATE_ABORT = 13
    HS_UPDATE_VALID = 14
    HS_UPDATE_RESTORED = 15
    KEY_REMOVED = 16
    STABLE_PG_WALK_SKIP = 17
    SKIP_UNMODIFIED = 18
    HS_GT_ONDISK = 19
    RECOVERY_RTS = 20
    HS_STOP_OBSOLETE = 21
    RECOVER_CKPT = 22
    HS_TREE_ROLLBACK = 23
    HS_TREE_SKIP = 24
    HS_ABORT_STOP = 25
    HS_RESTORE_TOMBSTONE = 26
    FILE_SKIP = 27
    SKIP_DAMAGE = 28
    HS_TRUNCATED = 29
    SHUTDOWN_RTS = 30
    END = 31

class Operation:
    def __init__(self, line):
        self.line = line

        # Extract the RTS message type, e.g. 'PAGE_ROLLBACK'.
        matches = re.search('\[WT_VERB_RTS\]\[DEBUG_\d+\]: \[(\w+)\]', line)
        if matches is None or matches.group(1) is None:
            raise Exception("Checker got a verbose RTS message in a format it didn't understand: {}"
                            .format(line))

        # 'PAGE_ROLLBACK' -> 'page_rollback' since we're using it to search for a function
        # and our names are all lowercase.
        name = matches.group(1).lower()

        # Search for a function in the class with the name we found. Our functions all start
        # with `__init_` so add that. The '_Operation' is our class name.
        ptr = getattr(self, '_Operation__init_' + name)
        if ptr is None:
            raise Exception("Checker got a verbose RTS message with a type it didn't understand!")

        # Call the function we found.
        ptr(line)

    def __repr__(self):
        return f"{self.__dict__}"

    def __extract_file(self, line):
        matches = re.search('(?:(file|tiered)):([\w_\.]+)', line)
        if matches is None:
            raise Exception(f"failed to extract a filename from {line}")
        return matches.group(1)

    def __extract_simple_timestamp(self, prefix, line):
        matches = re.search("{}=\((\d+), (\d+)\)".format(prefix), line)
        start = int(matches.group(1))
        stop = int(matches.group(2))
        return Timestamp(start, stop)

    def __extract_pointer(self, prefix, line):
        if os.name == 'nt':
            matches = re.search(f'{prefix}=([A-Za-z0-9]+)', line)
        else:
            matches = re.search(f'{prefix}=(0x[A-Za-z0-9]+)', line)

        if matches is None:
            raise Exception("failed to parse address string")

        return int(matches.group(1), 16)

    def __init_init(self, line):
        self.type = OpType.INIT

        matches = re.search('stable_timestamp=\((\d+), (\d+)\)', line)
        if matches is None:
            raise Exception("failed to parse init string")

        stable_start = int(matches.group(1))
        stable_stop = int(matches.group(2))
        self.stable = Timestamp(stable_start, stable_stop)

    def __init_tree(self, line):
        self.type = OpType.TREE
        self.file = self.__extract_file(line)

        matches = re.search('modified=(\w+)', line)
        self.modified = matches.group(1).lower() == "true"

        matches = re.search('durable_timestamp=\((\d+), (\d+)\).*>.*stable_timestamp=\((\d+), (\d+)\): (\w+)', line)
        durable_start = int(matches.group(1))
        durable_stop = int(matches.group(2))
        self.durable = Timestamp(durable_start, durable_stop)
        stable_start = int(matches.group(3))
        stable_stop = int(matches.group(4))
        self.stable = Timestamp(stable_start, stable_stop)
        self.durable_gt_stable = matches.group(5).lower() == "true"

        matches = re.search('has_prepared_updates=(\w+)', line)
        self.has_prepared_updates = matches.group(1).lower() == "true"

        matches = re.search('durable_timestamp_not_found=(\w+)', line)
        self.durable_ts_not_found = matches.group(1).lower() == "true"

        matches = re.search('txnid=(\d+).*>.*recovery_checkpoint_snap_min=(\d+): (\w+)', line)
        self.txnid = int(matches.group(1))
        self.recovery_ckpt_snap_min = int(matches.group(2))
        self.txnid_gt_recov_ckpt_snap_min = matches.group(3).lower() == "true"

    def __init_tree_logging(self, line):
        self.type = OpType.TREE_LOGGING
        self.file = self.__extract_file(line)

        matches = re.search('connection_logging_enabled=(\w+)', line)
        self.conn_logging_enabled = matches.group(1).lower() == "true"

        matches = re.search('btree_logging_enabled=(\w+)', line)
        self.btree_logging_enabled = matches.group(1).lower() == "true"

    def __init_page_rollback(self, line):
        self.type = OpType.PAGE_ROLLBACK
        self.file = self.__extract_file(line)
        self.addr = self.__extract_pointer('addr', line)

        matches = re.search('modified=(\w+)', line)
        self.modified = matches.group(1).lower() == "true"

    def __init_update_abort(self, line):
        self.type = OpType.UPDATE_ABORT
        self.file = self.__extract_file(line)

        matches = re.search('txnid=(\d+)', line)
        self.txnid = int(matches.group(1))
        matches = re.search('txnid_not_visible=(\w+)', line)
        self.txnid_not_visible = matches.group(1).lower() == "true"

        matches = re.search('stable_timestamp=\((\d+), (\d+)\).*<.*durable_timestamp=\((\d+), (\d+)\): (\w+)', line)
        stable_start = int(matches.group(1))
        stable_stop = int(matches.group(2))
        self.stable = Timestamp(stable_start, stable_stop)

        durable_start = int(matches.group(3))
        durable_stop = int(matches.group(4))
        self.durable = Timestamp(durable_start, durable_stop)

        self.stable_lt_durable = matches.group(5).lower() == "true"

        matches = re.search('prepare_state=(\w+)', line)
        self.prepare_state = PrepareState[matches.group(1)]

    def __init_page_abort_check(self, line):
        self.type = OpType.PAGE_ABORT_CHECK
        self.file = self.__extract_file(line)

        self.ref = self.__extract_pointer('ref', line)

        self.durable = self.__extract_simple_timestamp('durable_timestamp', line)

        matches = re.search('newest_txn=(\d+)', line)
        self.newest_txn = int(matches.group(1))

        matches = re.search('prepared_updates=(\w+)', line)
        self.has_prepared = matches.group(1).lower() == "true"

        matches = re.search('has_updates_need_abort=(\w+)', line)
        self.needs_abort = matches.group(1).lower() == "true"

    def __init_key_clear_remove(self, line):
        self.type = OpType.KEY_CLEAR_REMOVE
        self.file = self.__extract_file(line)

        self.restored_commit = self.__extract_simple_timestamp('commit_timestamp', line)
        self.restored_durable = self.__extract_simple_timestamp('durable_timestamp', line)
        self.restored_stable = self.__extract_simple_timestamp('stable_timestamp', line)

        matches = re.search('txnid=(\d+)', line)
        self.restored_txnid = int(matches.group(1))

        self.removed_commit = self.__extract_simple_timestamp('removed.*commit_timestamp', line)
        self.removed_durable = self.__extract_simple_timestamp('removed.*durable_timestamp', line)

        matches = re.search('removed.*txnid=(\d+)', line)
        self.removed_txnid = int(matches.group(1))

        matches = re.search('removed.*prepared=(\w+)', line)
        self.removed_prepared = matches.group(1).lower() == "true"

    def __init_ondisk_kv_remove(self, line):
        self.type = OpType.ONDISK_KV_REMOVE
        self.file = self.__extract_file(line)

        matches = re.search('tombstone=(\w+)', line)
        self.tombstone = matches.group(1).lower() == "true"

        matches = re.search('key=(\d+)', line)
        self.key = int(matches.group(1))

    def __init_shutdown_init(self, line):
        self.type = OpType.SHUTDOWN_INIT

        self.stable = self.__extract_simple_timestamp('stable_timestamp', line)

    def __init_shutdown_rts(self, line):
        self.type = OpType.SHUTDOWN_RTS

        matches = re.search('performing shutdown rollback to stable failed with code (\w+)', line)
        self.shutdown_rts_error = matches.group(1).lower() != "0"

    def __init_tree_skip(self, line):
        self.type = OpType.TREE_SKIP
        self.file = self.__extract_file(line)

        self.durable = self.__extract_simple_timestamp('durable_timestamp', line)
        self.stable = self.__extract_simple_timestamp('stable_timestamp', line)

        matches = re.search('txnid=(\d+)', line)
        self.txnid = int(matches.group(1))

    def __init_skip_del_null(self, line):
        self.type = OpType.SKIP_DEL_NULL
        self.file = self.__extract_file(line)
        self.ref = self.__extract_pointer('ref', line)

    def __init_ondisk_abort_tw(self, line):
        self.type = OpType.ONDISK_ABORT_TW
        self.file = self.__extract_file(line)

        matches = re.search('time_window=\((\d+), (\d+)\)/\((\d+), (\d+)\)/(\d+)', line)
        start_start = int(matches.group(1))
        start_end = int(matches.group(2))
        self.start = Timestamp(start_start, start_end)
        durable_start_start = int(matches.group(3))
        durable_start_end = int(matches.group(4))
        self.durable_start = Timestamp(durable_start_start, durable_start_end)
        self.start_txn = int(matches.group(5))

        matches = re.search('durable_timestamp > stable_timestamp: (\w+)', line)
        self.durable_gt_stable = matches.group(1).lower() == "true"

        matches = re.search('txnid_not_visible=(\w+)', line)
        self.txnid_not_visible = matches.group(1).lower == "true"

        matches = re.search('tw_has_no_stop_and_is_prepared=(\w+)', line)
        self.tw_has_no_stop_and_is_prepared = matches.group(1).lower == "true"

    def __init_ondisk_key_rollback(self, line):
        self.type = OpType.ONDISK_KEY_ROLLBACK
        self.file = self.__extract_file(line)

        matches = re.search('key=(\d+)', line)
        self.key = int(matches.group(1))

    def __init_hs_update_abort(self, line):
        self.type = OpType.HS_UPDATE_ABORT
        self.file = self.__extract_file(line)

        matches = re.search('time_window=start: \((\d+), (\d+)\)/\((\d+), (\d+)\)/(\d+) stop: \((\d+), (\d+)\)/\((\d+), (\d+)\)/(\d+)', line)

        durable_start_start = int(matches.group(1))
        durable_start_end = int(matches.group(2))
        self.durable_start = Timestamp(durable_start_start, durable_start_end)
        start_start = int(matches.group(3))
        start_end = int(matches.group(4))
        self.start = Timestamp(start_start, start_end)
        self.start_txn = int(matches.group(5))

        durable_stop_start = int(matches.group(6))
        durable_stop_end = int(matches.group(7))
        self.durable_stop = Timestamp(durable_start_start, durable_start_end)
        stop_start = int(matches.group(8))
        stop_end = int(matches.group(9))
        self.stop = Timestamp(start_start, start_end)
        self.stop_txn = int(matches.group(10))

        matches = re.search('type=(\w+)', line)
        self.update_type = UpdateType[matches.group(1)]

        self.stable = self.__extract_simple_timestamp('stable_timestamp', line)

    def __init_hs_update_valid(self, line):
        self.type = OpType.HS_UPDATE_VALID
        self.file = self.__extract_file(line)

        matches = re.search('time_window=start: \((\d+), (\d+)\)/\((\d+), (\d+)\)/(\d+) stop: \((\d+), (\d+)\)/\((\d+), (\d+)\)/(\d+)', line)

        durable_start_start = int(matches.group(1))
        durable_start_end = int(matches.group(2))
        self.durable_start = Timestamp(durable_start_start, durable_start_end)
        start_start = int(matches.group(3))
        start_end = int(matches.group(4))
        self.start = Timestamp(start_start, start_end)
        self.start_txn = int(matches.group(5))

        durable_stop_start = int(matches.group(6))
        durable_stop_end = int(matches.group(7))
        self.durable_stop = Timestamp(durable_start_start, durable_start_end)
        stop_start = int(matches.group(8))
        stop_end = int(matches.group(9))
        self.stop = Timestamp(start_start, start_end)
        self.stop_txn = int(matches.group(10))

        matches = re.search('type=(\w+)', line)
        self.update_type = UpdateType[matches.group(1)]

        self.stable = self.__extract_simple_timestamp('stable_timestamp', line)

    def __init_hs_update_restored(self, line):
        self.type = OpType.HS_UPDATE_VALID
        self.file = self.__extract_file(line)

        matches = re.search('txnid=(\d+)', line)
        self.txnid = int(matches.group(1))

        self.start = self.__extract_simple_timestamp('start_ts', line)
        self.durable = self.__extract_simple_timestamp('durable_ts', line)

    def __init_key_removed(self, line):
        self.type = OpType.KEY_REMOVED
        self.file = self.__extract_file(line)

    def __init_stable_pg_walk_skip(self, line):
        self.type = OpType.KEY_REMOVED
        self.file = self.__extract_file(line)
        self.addr = self.__extract_pointer('ref', line)

    def __init_skip_unmodified(self, line):
        self.type = OpType.SKIP_UNMODIFIED
        self.file = self.__extract_file(line)
        self.addr = self.__extract_pointer('ref', line)

    def __init_hs_gt_ondisk(self, line):
        self.type = OpType.HS_GT_ONDISK
        self.file = self.__extract_file(line)

        matches = re.search('time_window=start: \((\d+), (\d+)\)/\((\d+), (\d+)\)/(\d+) stop: \((\d+), (\d+)\)/\((\d+), (\d+)\)/(\d+)', line)

        durable_start_start = int(matches.group(1))
        durable_start_end = int(matches.group(2))
        self.durable_start = Timestamp(durable_start_start, durable_start_end)
        start_start = int(matches.group(3))
        start_end = int(matches.group(4))
        self.start = Timestamp(start_start, start_end)
        self.start_txn = int(matches.group(5))

        durable_stop_start = int(matches.group(6))
        durable_stop_end = int(matches.group(7))
        self.durable_stop = Timestamp(durable_start_start, durable_start_end)
        stop_start = int(matches.group(8))
        stop_end = int(matches.group(9))
        self.stop = Timestamp(start_start, start_end)
        self.stop_txn = int(matches.group(10))

        matches = re.search('type=(\w+)', line)
        self.update_type = UpdateType[matches.group(1)]

    def __init_recovery_rts(self, line):
        self.type = OpType.RECOVERY_RTS

        self.stable = self.__extract_simple_timestamp('stable_timestamp', line)
        self.oldest = self.__extract_simple_timestamp('oldest_timestamp', line)

    def __init_hs_stop_obsolete(self, line):
        self.type = OpType.HS_STOP_OBSOLETE
        self.file = self.__extract_file(line)

        matches = re.search('time_window=start: \((\d+), (\d+)\)/\((\d+), (\d+)\)/(\d+) stop: \((\d+), (\d+)\)/\((\d+), (\d+)\)/(\d+)', line)
        durable_start_start = int(matches.group(1))
        durable_start_end = int(matches.group(2))
        self.durable_start = Timestamp(durable_start_start, durable_start_end)
        start_start = int(matches.group(3))
        start_end = int(matches.group(4))
        self.start = Timestamp(start_start, start_end)
        self.start_txn = int(matches.group(5))

        durable_stop_start = int(matches.group(6))
        durable_stop_end = int(matches.group(7))
        self.durable_stop = Timestamp(durable_start_start, durable_start_end)
        stop_start = int(matches.group(8))
        stop_end = int(matches.group(9))
        self.stop = Timestamp(start_start, start_end)
        self.stop_txn = int(matches.group(10))

        self.pinned = self.__extract_simple_timestamp('pinned_timestamp', line)

    def __init_recover_ckpt(self, line):
        self.type = OpType.RECOVER_CKPT

        matches = re.search('snapshot_min=(\d+)', line)
        self.snapshot_min = int(matches.group(1))

        matches = re.search('snapshot_max=(\d+)', line)
        self.snapshot_max = int(matches.group(1))

        matches = re.search('snapshot_count=(\d+)', line)
        self.snapshot_count = int(matches.group(1))

    def __init_hs_tree_rollback(self, line):
        self.type = OpType.HS_TREE_ROLLBACK
        self.file = self.__extract_file(line)

        self.durable = self.__extract_simple_timestamp('durable_timestamp', line)

    def __init_hs_tree_skip(self, line):
        self.type = OpType.HS_TREE_SKIP
        self.file = self.__extract_file(line)

        self.durable = self.__extract_simple_timestamp('durable_timestamp', line)
        self.stable = self.__extract_simple_timestamp('stable_timestamp', line)

    def __init_hs_abort_stop(self, line):
        self.type = OpType.HS_ABORT_STOP
        self.file = self.__extract_file(line)

        matches = re.search('start_durable/commit_timestamp=\((\d+), (\d+)\), \((\d+), (\d+)\)', line)
        durable_start_start = int(matches.group(1))
        durable_start_stop = int(matches.group(2))
        self.durable_start = Timestamp(durable_start_start, durable_start_stop)
        commit_start_start = int(matches.group(3))
        commit_start_stop = int(matches.group(4))
        self.commit_start = Timestamp(commit_start_start, commit_start_stop)

        matches = re.search('stop_durable/commit_timestamp=\((\d+), (\d+)\), \((\d+), (\d+)\)', line)
        durable_stop_start = int(matches.group(1))
        durable_stop_stop = int(matches.group(2))
        self.durable_stop = Timestamp(durable_start_start, durable_stop_start)
        commit_stop_start = int(matches.group(3))
        commit_stop_stop = int(matches.group(4))
        self.commit_stop = Timestamp(commit_stop_start, commit_stop_stop)

        self.stable = self.__extract_simple_timestamp('stable_timestamp', line)

    def __init_hs_restore_tombstone(self, line):
        self.type = OpType.HS_RESTORE_TOMBSTONE
        self.file = self.__extract_file(line)

        matches = re.search('txnid=(\d+)', line)
        self.txnid = int(matches.group(1))

        self.start = self.__extract_simple_timestamp('start_ts', line)
        self.durable = self.__extract_simple_timestamp('durable_ts', line)

    def __init_file_skip(self, line):
        self.type = OpType.TREE_SKIP
        self.file = self.__extract_file(line)

    def __init_skip_damage(self, line):
        self.type = OpType.SKIP_DAMAGE
        self.file = self.__extract_file(line)
        self.corrupted = "corrupt" in line

    def __init_hs_truncated(self, line):
        self.type = OpType.HS_TRUNCATED

        matches = re.search('btree=(\d+)', line)
        self.btree_id = int(matches.group(1))


    def __init_end(self, line):
        self.type = OpType.END
