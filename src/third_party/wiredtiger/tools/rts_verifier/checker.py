#!/usr/bin/env python3

from basic_types import Page, PrepareState, Timestamp, Tree
from operation import OpType

class Checker:
    def __init__(self):
        self.stable = None
        self.visited_trees = set()
        self.visited_pages = set()

    def apply(self, operation):
        # The type's name will be something like 'PAGE_ROLLBACK'.
        opname = operation.type.name.lower()

        # Construct the name of the function call from the opname, e.g. __apply_check_page_rollback.
        # The '_Checker' bit is the class name.
        ptr = getattr(self, '_Checker__apply_check_' + opname)
        if ptr is None:
            raise Exception("Checker got an operation type it didn't understand!")

        # Call the function we found.
        ptr(operation)

    def __apply_check_init(self, operation):
        # reset a bunch of internal state
        self.stable = operation.stable
        self.visited_trees = set()
        self.visited_pages = set()
        self.current_tree = None

    def __apply_check_tree(self, operation):
        tree = Tree(operation.file)
        if tree in self.visited_trees:
            raise Exception(f"visited file {operation.file} again, operation={operation}")
        self.visited_trees.add(tree)
        self.current_tree = tree

        if not(operation.modified or
               operation.durable_gt_stable or
               operation.has_prepared_updates or
               operation.durable_ts_not_found or
               operation.txnid_gt_recov_ckpt_snap_min):
            raise Exception(f"unnecessary visit to {operation.file}")

        if operation.durable_gt_stable and not operation.durable > operation.stable:
            raise Exception(f"incorrect timestamp comparison: thought {operation.durable} > {operation.stable}, but it isn't")
        if not operation.durable_gt_stable and not operation.stable >= operation.durable:
            raise Exception(f"incorrect timestamp comparison: thought {operation.durable} <= {operation.stable}, but it isn't")

        if operation.durable_ts_not_found and operation.durable != Timestamp(0, 0):
            raise Exception("we thought we didn't have a durable timestamp, but we do")

        if operation.stable != self.stable:
            raise Exception(f"stable timestamp spuriously changed from {self.stable} to {operation.stable} while rolling back {operation.file}")

    def __apply_check_tree_logging(self, operation):
        # TODO expand this out
        # if operation.file != self.current_tree.file:
        #     raise Exception(f"spurious visit to {operation.file}")

        if self.current_tree is not None and self.current_tree.logged is not None and self.current_tree.logged != operation.btree_logging_enabled:
            raise Exception(f"{operation.file} spuriously changed btree logging state")

        if self.current_tree is not None:
            self.current_tree.logged = operation.btree_logging_enabled

    def __apply_check_page_rollback(self, operation):
        # TODO expand this out - not always spurious for the history store.
        # if operation.file != self.current_tree.file:
        #     raise Exception(f"spurious visit to {operation.file}")

        page = Page(operation.addr)
        page.modified = operation.modified
        # if page in self.visited_pages:
        #     raise Exception(f"visited page {operation.addr} again")
        self.visited_pages.add(page)

    def __apply_check_update_abort(self, operation):
        if operation.file != self.current_tree.file:
            raise Exception(f"spurious visit to {operation.file}")

        if not(operation.txnid_not_visible or
               operation.stable_lt_durable or
               operation.prepare_state == PrepareState.WT_PREPARE_INPROGRESS):
            raise Exception(f"aborted update with txnid={operation.txnid} for no reason")

        if operation.stable_lt_durable and not operation.stable < operation.durable:
            raise Exception(f"incorrect timestamp comparison: thought {operation.stable} < {operation.durable}, but it isn't")
        if not operation.stable_lt_durable and not operation.stable >= operation.durable:
            raise Exception(f"incorrect timestamp comparison: thought {operation.stable} >= {operation.durable}, but it isn't")

    def __apply_check_page_abort_check(self, operation):
        # TODO expand this out
        # if operation.file != self.current_tree.file:
        #     raise Exception(f"spurious visit to {operation.file}, {operation=}")

        # TODO print session recovery flags to check the other way this can be wrong
        should_rollback = (operation.durable <= self.stable) or operation.has_prepared
        # if should_rollback and not operation.needs_abort:
        #     raise Exception(f"incorrectly skipped rolling back page with ref={operation.ref}")

        # TODO be a little smarter about storing page state. the decision we make about rolling back
        # can be stored, and checked against future log lines to e.g. make sure we don't change our
        # mind at some point.

        # TODO can  probably use the txn ID somehow.

    def __apply_check_key_clear_remove(self, operation):
        if operation.file != self.current_tree.file:
            raise Exception(f"spurious visit to {operation.file}, operation={operation}")

        # TODO print session recovery flags to check the other way this can be wrong
        should_abort = (operation.removed_durable <= self.stable) or operation.removed_prepared
        if should_abort and not operation.needs_abort:
            raise Exception(f"incorrectly skipped rolling back page with ref={operation.ref}")

        # TODO can likely expand on these
        if operation.restored_stable != self.stable:
            raise Exception("stable timestamp spuriously moved forward")

        if operation.removed_commit < self.stable:
            raise Exception("aborted an update from before the stable timestamp?!")

    def __apply_check_ondisk_kv_remove(self, operation):
        # TODO expand this out
        # if operation.file != self.current_tree.file:
        #     raise Exception(f"spurious visit to {operation.file}, {operation=}")
        pass

    def __apply_check_shutdown_init(self, operation):
        # TODO expand this out
        pass

    def __apply_check_shutdown_rts(self, operation):
        # if operation.file != self.current_tree.file:
        #     raise Exception(f"spurious visit to {operation.file}")

        # if operation.shutdown_rts_error:
        #     raise Exception("Rollback to stable during shutdown failed")
        pass

    def __apply_check_tree_skip(self, operation):
        # TODO expand this out
        pass

    def __apply_check_skip_del_null(self, operation):
        # TODO expand this out
        pass

    def __apply_check_ondisk_abort_tw(self, operation):
        # TODO expand this out
        pass

    def __apply_check_ondisk_key_rollback(self, operation):
        # TODO expand this out
        pass

    def __apply_check_hs_update_abort(self, operation):
        # TODO expand this out
        pass

    def __apply_check_hs_update_valid(self, operation):
        # TODO expand this out
        pass

    def __apply_check_hs_update_restored(self, operation):
        # TODO expand this out
        pass

    def __apply_check_key_removed(self, operation):
        # TODO expand this out
        pass

    def __apply_check_stable_pg_walk_skip(self, operation):
        # TODO expand this out
        pass

    def __apply_check_skip_unmodified(self, operation):
        # TODO expand this out
        pass

    def __apply_check_hs_gt_ondisk(self, operation):
        # TODO expand this out
        pass

    def __apply_check_recovery_rts(self, operation):
        # TODO expand this out
        pass

    def __apply_check_hs_stop_obsolete(self, operation):
        # TODO expand this out
        pass

    def __apply_check_recover_ckpt(self, operation):
        # TODO expand this out
        pass

    def __apply_check_hs_tree_rollback(self, operation):
        # TODO expand this out
        pass

    def __apply_check_hs_tree_skip(self, operation):
        # TODO expand this out
        pass

    def __apply_check_hs_abort_stop(self, operation):
        # TODO expand this out
        pass

    def __apply_check_hs_restore_tombstone(self, operation):
        # TODO expand this out
        pass

    def __apply_check_file_skip(self, operation):
        # TODO expand this out
        pass

    def __apply_check_skip_damage(self, operation):
        # TODO expand this out
        pass

    def __apply_check_hs_truncated(self, operation):
        # TODO expand this out
        pass

    def __apply_check_end(self, operation):
        # TODO expand this out
        pass
