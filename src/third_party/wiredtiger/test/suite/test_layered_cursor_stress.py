#!/usr/bin/env python3
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
# test_layered_cursor_stress.py
#
# Seed-driven stress test for layered cursors.
#
# Reference comparison: per connection (leader and follower), one session holds a plain reference table (asc)
# and the layered table under test (dsc). Every write is mirrored to both; every read is run on
# both cursors and the (error code, key, value) compared. The plain reference is real WiredTiger,
# so it is a correct reference for read_timestamps / isolation / prepare with no modeling.
#
# The seed set is fixed, and the test is single threaded, so a run is deterministic and a failure
# repeats on re-run. Every chosen event is appended to a per-seed trace file.

# FIXME-WT-17838: things left unimplemented for this test:
#
# - The most important would be a separate configuration that runs the test with a random seed and
#   configuration, extending coverage with every run. Since the test is completely deterministic,
#   every failure is easily reproducible, so after finding a new issue we can introduce a new fixed
#   configuration, as we usually do with test/format.
# - The second most important would be to run the bulk scenarios through a separate cursor in a
#   separate session and transaction, so the inserts/removes arrive like writes from another thread
#   (a production-like concurrent follower writer). For the asc-vs-dsc comparison to stay valid the
#   two tested cursors must always share one snapshot, so this first needs the no-txn (autocommit
#   read-committed) read ops disabled -- otherwise asc and dsc refresh at different points.
#   Especially interesting under the read-committed and read-uncommitted isolation levels.
# - Two modes for the eviction scenario: not only evict everything right after the checkpoint, but
#   also do partial evictions of 20/40/60/80/100% with a cursor open, so we remove only the ingest
#   entries that are permitted to be removed.
#
# The next points are more about extending the coverage of individual operations:
#  - Test tombstone-prefixed values (to cover __wt_clayered_deleted encode/decode).
#  - Add operations with overwrite on/off.
#  - Add setting bounds to cursors.

import math, os, random
from dataclasses import dataclass, field
from enum import Enum
import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

class Txn(Enum):
    # The generator's current transaction context; the value is the isolation= string where one
    # applies. READ_TIMESTAMP is an as-of-past read; write_allowed() says which contexts permit writes.
    NO = 'no'                              # autocommit -- no explicit transaction
    SNAPSHOT = 'snapshot'                  # read-write snapshot transaction
    READ_COMMITTED = 'read-committed'      # read-only
    READ_UNCOMMITTED = 'read-uncommitted'  # read-only
    READ_TIMESTAMP = 'read-timestamp'      # snapshot isolation + an as-of-past read; read-only

def write_allowed(txn):
    return txn in (Txn.NO, Txn.SNAPSHOT)

# The default Weights() below are a balanced starting point; the tuned per-theme mixes live in
# WORKLOAD_PROFILES and are what the suite actually runs.
# FIXME-WT-17827: the follower layered cursor mishandles operating on a just-removed cursor -- a repeat
# remove of an already-deleted key leaves a stale position (a later iterate then diverges) and modify on
# a deleted slot aborts. So op_pos_remove resets the dsc cursor in that case (see there) and there is no
# modify op; revisit both once WT-17827 lands.
# FIXME-WT-17825: add prepared transactions once fixed (prepare misbehaves on the follower layered cursor).

@dataclass(frozen=True)
class TxnModeWeights:
    # op_txn_begin sub-weights: which flavor to begin (snapshot is read-write; the rest read-only,
    # read_timestamp = an as-of-past read), and how to end an open txn (commit vs rollback).
    snapshot: float = 72
    read_committed: float = 16
    read_uncommitted: float = 12
    read_timestamp: float = 30
    commit: float = 90
    rollback: float = 10

@dataclass(frozen=True)
class SearchKeyWeights:
    # op_search / op_search_near pick an existing key, or a missing one (absent from py_table -- often
    # a removed key, exercising tombstones and the search_near neighbor logic).
    existing: float = 50
    missing: float = 50

@dataclass(frozen=True)
class RemoveKeyWeights:
    # op_remove picks an existing key (a real delete that mutates state) or a missing one (layered and
    # reference must return the same not-found result -- removing an absent/tombstoned key).
    existing: float = 80
    missing: float = 20

@dataclass(frozen=True)
class BulkRemoveWeights:
    # scen_bulk_remove deletes a contiguous 40/80/100% range either by per-key remove or range truncate.
    # FIXME-WT-XXXX: truncate over-truncates on the follower layered table -- a key re-inserted inside a
    # prior truncate range is lost once it drains to stable. Disabled (weight 0) until fixed; raise it then.
    remove: float = 100
    truncate: float = 0

@dataclass(frozen=True)
class Weights:
    # Relative weights: an op's probability is its weight / the sum of the legal weights at the step.
    # Position-holding ops (reads + positional writes) carry the big weights so chains stay long and
    # the cursor is usually positioned -- the heart of the test. Weights may be fractional, so a long
    # run can make a rare op (evict / advance_checkpoint) sub-1% (e.g. 0.1) without inflating the rest.
    next: float = 40
    prev: float = 40
    search: float = 12
    search_near: float = 10
    pos_update: float = 14
    pos_remove: float = 8
    put: float = 6
    remove: float = 2
    reset: float = 2
    full_scan: float = 4
    bulk_insert: float = 4
    bulk_remove: float = 2
    advance_checkpoint: float = 6
    evict: float = 6
    txn_begin: float = 8
    bulk_insert_frac: float = 0.1 # fraction of the pool each scen_bulk_insert batch covers
    txn_mode: TxnModeWeights = field(default_factory=TxnModeWeights)
    search_key: SearchKeyWeights = field(default_factory=SearchKeyWeights)
    remove_key: RemoveKeyWeights = field(default_factory=RemoveKeyWeights)
    bulk_remove_mode: BulkRemoveWeights = field(default_factory=BulkRemoveWeights)

@dataclass(frozen=True)
class Op:
    fn: object                    # the op method, called as fn(nodes, rnd, trace); the dispatch identity
    weight: float                   # relative frequency among the legal ops at each step
    needs_position: bool = False  # cursor must be positioned (positional writes)
    is_write: bool = False        # a logical write (illegal in a read-only transaction)
    no_txn: bool = True           # legal with no open txn (autocommit)
    in_txn: bool = True           # legal inside an open txn

class EventTrace:
    # Append-only, flushed-per-line record of every chosen event for a seed.
    def __init__(self, path, header):
        self.path = path
        self._f = open(path, 'w')
        self._n = 0
        self._f.write('# %s\n' % header)
        self._f.flush()

    def log(self, event):
        self._f.write('%d: %s\n' % (self._n, event))
        self._f.flush()
        self._n += 1

    def close(self):
        self._f.close()

class Node:
    # One connection's view: a layered table (dsc) and a plain reference table (asc), one cursor each
    # in one session, used for both reads and writes. Sharing the cursor keeps layered and reference
    # in lockstep and leaves the cursor positioned after a write (toward long-lived positioned chains).
    def __init__(self, conn, session, dsc_uri, asc_uri):
        self.conn = conn
        self.session = session
        self.dsc_uri = dsc_uri
        self.asc_uri = asc_uri
        self.dsc_c = session.open_cursor(dsc_uri)
        self.asc_c = session.open_cursor(asc_uri)

    def reset_all(self):
        self.dsc_c.reset()
        self.asc_c.reset()

    def close(self):
        self.dsc_c.close()
        self.asc_c.close()

class State:
    # The model the generator reasons about, never the reference -- it only drives op generation.
    # Connection-global fields (timestamps, coverage counters) persist across sequences in a
    # multi-seed run; new_sequence() resets the per-sequence fields for each fresh set of tables.
    def __init__(self):
        self.ts = 0              # monotonic commit/stable timestamp
        self.wseq = 0            # monotonic write counter, for unique values
        self.oldest_ts = 0       # current oldest_timestamp; floor for legal read_timestamps
        self.last_advance_checkpoint_ts = 0  # self.ts at the previous advance_checkpoint; oldest lags to here
        self.n_positional = 0    # positional update/remove ops applied (long-lived-chain guard)
        self.n_read_ts = 0       # as-of-past read transactions opened (read_timestamp guard)
        self.n_iso_rc = 0        # read-committed transactions opened (isolation guard)
        self.n_iso_ru = 0        # read-uncommitted transactions opened (isolation guard)
        self.n_full_scan = 0     # full-table cross-checks run (scen_full_scan guard)
        self.n_bulk_insert = 0   # bulk-insert batches applied (scen_bulk_insert guard)
        self.n_bulk_remove = 0   # bulk-remove/truncate batches applied (scen_bulk_remove guard)
        # Chain-length + fill/empty instrumentation (connection-global, across all seeds).
        self.chain_total = 0     # sum of completed positioned-run lengths
        self.chain_count = 0     # number of completed positioned runs
        self.n_reached_full = 0         # times py_table rose to hold every pool key
        self.n_reached_empty = 0        # times py_table transitioned to empty
        self.max_n = 0                  # DIAGNOSTIC: largest py_table size seen
        self.n_advance = 0; self.n_evict = 0   # DIAGNOSTIC
        self.op_counts = {}             # DIAGNOSTIC: per-op fire counts (workload diversity)
        self.new_sequence()

    def new_sequence(self):
        self.chain_run = 0             # current run of consecutive positioned ops
        self.py_table = {}             # logical key->value, for op generation only
        self.prev_full = False         # py_table was full on the previous op (rising-edge detection)
        self.prev_empty = True         # py_table was empty on the previous op (starts empty, not counted)
        self.cur_pos = None            # key both cursor pairs are positioned on (None = unpositioned)
        self.txn = Txn.NO              # current transaction context (NO = autocommit)
        self.txn_wrote = False         # the open txn has performed at least one write
        self.txn_read_ts = None        # the as-of timestamp when txn is READ_TIMESTAMP, else None
        self.py_table_snapshot = None  # py_table as of begin, restored on rollback

# A diverse, comprehensive set of workload profiles, each a balanced (no single-op-dominated) mix
# tilted toward one merge sub-area. Each runs quick (tens of seconds) over a few seeds; together they
# cover the iterate-merge, point-lookup-merge, positioned-write, transaction/as-of-past, big-delete,
# and stable-drain paths. Every profile reaches a full pool, empties it, and reads a real fraction
# from stable (asserted by assert_workload_coverage).
WORKLOAD_PROFILES = {
    'mixed': dict(n_keys=55, n_ops=22000, seeds=3, weights=Weights(
        next=18, prev=18, search=16, search_near=14, pos_update=10, pos_remove=4, put=4, remove=2,
        reset=1, full_scan=0.5, advance_checkpoint=1.6, evict=2.2, bulk_insert=1.2, bulk_remove=0.4,
        txn_begin=4, bulk_insert_frac=0.4, search_key=SearchKeyWeights(existing=80, missing=20),
        txn_mode=TxnModeWeights(read_timestamp=40))),
    'iterate': dict(n_keys=55, n_ops=16000, seeds=3, weights=Weights(
        next=35, prev=35, search=12, search_near=10, pos_update=8, pos_remove=3, put=3, remove=1.5,
        reset=0.5, full_scan=0.4, advance_checkpoint=5.0, evict=5.0, bulk_insert=1.0, bulk_remove=0.4,
        txn_begin=3, bulk_insert_frac=0.4, search_key=SearchKeyWeights(existing=85, missing=15))),
    'lookup': dict(n_keys=55, n_ops=20000, seeds=3, weights=Weights(
        next=15, prev=15, search=31, search_near=22, pos_update=9, pos_remove=3, put=3, remove=2,
        reset=0.5, full_scan=0.4, advance_checkpoint=2.0, evict=2.5, bulk_insert=1.5, bulk_remove=0.4,
        txn_begin=3, bulk_insert_frac=0.4, search_key=SearchKeyWeights(existing=55, missing=45))),
    'write_churn': dict(n_keys=55, n_ops=14000, seeds=3, weights=Weights(
        next=11, prev=11, search=10, search_near=7, pos_update=24, pos_remove=14, put=13, remove=9,
        reset=0.5, full_scan=0.4, advance_checkpoint=3.5, evict=5.0, bulk_insert=2.0, bulk_remove=0.6,
        txn_begin=3, bulk_insert_frac=0.4, search_key=SearchKeyWeights(existing=70, missing=30),
        remove_key=RemoveKeyWeights(existing=70, missing=30))),
    'txn': dict(n_keys=55, n_ops=20000, seeds=3, weights=Weights(
        next=18, prev=18, search=16, search_near=10, pos_update=10, pos_remove=4, put=4, remove=2,
        reset=0.5, full_scan=0.4, advance_checkpoint=1.5, evict=3.5, bulk_insert=1.5, bulk_remove=0.4,
        txn_begin=18, bulk_insert_frac=0.4, search_key=SearchKeyWeights(existing=80, missing=20),
        txn_mode=TxnModeWeights(snapshot=40, read_committed=25, read_uncommitted=25, read_timestamp=90))),
    'bulk_swing': dict(n_keys=55, n_ops=6000, seeds=3, weights=Weights(
        next=18, prev=18, search=16, search_near=10, pos_update=8, pos_remove=4, put=4, remove=2,
        reset=0.5, full_scan=0.5, advance_checkpoint=15.0, evict=18.0, bulk_insert=5, bulk_remove=2.5,
        txn_begin=3, bulk_insert_frac=0.5, search_key=SearchKeyWeights(existing=80, missing=20))),
    'merge_drain': dict(n_keys=55, n_ops=14000, seeds=3, weights=Weights(
        next=20, prev=20, search=18, search_near=12, pos_update=6, pos_remove=2, put=2, remove=1,
        reset=0.4, full_scan=0.4, advance_checkpoint=4.0, evict=6.0, bulk_insert=2.0, bulk_remove=0.4,
        txn_begin=3, bulk_insert_frac=0.4, search_key=SearchKeyWeights(existing=85, missing=15))),
}

@disagg_test_class
class test_layered_cursor_stress(wttest.WiredTigerTestCase):
    conn_base_config = ',create,cache_size=1GB,statistics=(all),'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    profiles = [(name, dict(profile=name)) for name in WORKLOAD_PROFILES]
    scenarios = make_scenarios(disagg_storages, profiles)

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    # --- cluster setup ---------------------------------------------------

    def setup_connections(self, weights, n_keys=90):
        self.conn_follow = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")')
        self.session_follow = self.conn_follow.open_session('')
        self.state = State()
        self.weights = weights
        # Candidate keys spread by 10 so search_near targets fall between them.
        self.pool = list(range(100, 100 + n_keys * 10, 10))
        self.ops = self._build_ops(weights)   # the workload table (Op rows -> op methods)

        # Advancing to an unchanged checkpoint logs an expected WARNING.
        self.ignoreStdoutPattern('Picking up the same checkpoint again')

    def _build_ops(self, weights):
        # One Op row per op, pairing the op method (the dispatch identity) with its legality tags and
        # the weight copied from the Weights field it belongs to, so run_op samples plain rows.

        # Scenarios are ops that don't logically match to any cursor operation.
        return [
            Op(self.op_next,        weights.next),
            Op(self.op_prev,        weights.prev),
            Op(self.op_search,      weights.search),
            Op(self.op_search_near, weights.search_near),
            Op(self.op_pos_update,  weights.pos_update, needs_position=True, is_write=True),
            Op(self.op_pos_remove,  weights.pos_remove, needs_position=True, is_write=True),
            Op(self.op_put,         weights.put,        is_write=True),
            Op(self.op_remove,      weights.remove,     is_write=True),
            Op(self.op_reset,       weights.reset),
            Op(self.scen_full_scan, weights.full_scan),
            Op(self.scen_bulk_insert, weights.bulk_insert, is_write=True),
            Op(self.scen_bulk_remove, weights.bulk_remove, is_write=True),
            Op(self.scen_advance_checkpoint, weights.advance_checkpoint, in_txn=False),
            Op(self.scen_evict,     weights.evict,      in_txn=False),
            Op(self.op_txn_begin,   weights.txn_begin),
        ]

    def make_nodes(self, tag):
        # The layered table must share a name across connections so the follower picks up
        # the leader's checkpoint; the plain reference tables are independent per connection.
        dsc = 'layered:lcs_dsc_%s' % tag
        asc = 'table:lcs_asc_%s' % tag
        cfg = 'key_format=i,value_format=S'
        for session in (self.session, self.session_follow):
            session.create(dsc, cfg)
            session.create(asc, cfg)
        self.state.new_sequence()
        return [Node(self.conn, self.session, dsc, asc),
                Node(self.conn_follow, self.session_follow, dsc, asc)]

    # --- write protocol --------------------------------------------------

    def new_value(self, key):
        self.state.wseq += 1
        return 'v%d.%d' % (key, self.state.wseq)

    def _txn_scope(self, nodes, do_node):
        # The txn wrapper shared by writes: run do_node(n) per node inside the open txn (joining it),
        # else wrap each node's work in its own timestamped txn (layered = ordered write timestamps).
        if self.state.txn is not Txn.NO:
            for n in nodes:
                do_node(n)
            self.state.txn_wrote = True
        else:
            self.state.ts += 1
            for n in nodes:
                n.session.begin_transaction()
                do_node(n)
                n.session.commit_transaction('commit_timestamp=' + self.timestamp_str(self.state.ts))

    def _write_txn(self, nodes, do, label):
        notfound = False
        def step(n):
            nonlocal notfound
            ret_dsc = do(n.dsc_c); ret_asc = do(n.asc_c)
            self.assertEqual(ret_dsc, ret_asc, '%s result differs layered=%r reference=%r (trace %s)'
                             % (label, ret_dsc, ret_asc, self.trace.path))
            if ret_dsc == wiredtiger.WT_NOTFOUND:
                notfound = True
        self._txn_scope(nodes, step)
        return notfound

    def _positional(self, nodes, do, label):
        # Positional update/remove off the cursor's held position; clear cur_pos if the write missed.
        # Returns True if the write hit (so the caller can update the model only on success).
        notfound = self._write_txn(nodes, do, label)
        if notfound:
            self.state.cur_pos = None
        self.state.n_positional += 1
        return not notfound

    def commit_txn(self, nodes):
        # A txn that wrote needs a commit_timestamp (layered = ordered write timestamps).
        commit_cfg = ''
        if self.state.txn_wrote:
            self.state.ts += 1
            commit_cfg = 'commit_timestamp=' + self.timestamp_str(self.state.ts)
        for n in nodes:
            n.session.commit_transaction(commit_cfg)

        # FIXME-WT-17830: a follower layered cursor held across an as-of-past txn commit
        # fails to advance (stays on its key instead of moving). Reset works around it; remove once fixed.
        if self.state.txn_read_ts is not None:
            for n in nodes:
                n.reset_all()
            self.state.cur_pos = None
        self._reset_txn_state()

    def rollback_txn(self, nodes):
        for n in nodes:
            n.session.rollback_transaction()
        self.state.py_table = self.state.py_table_snapshot   # undo the txn's logical writes
        self.state.cur_pos = None                            # rollback resets the session's cursors
        self._reset_txn_state()

    def _reset_txn_state(self):
        self.state.txn = Txn.NO
        self.state.txn_wrote = False
        self.state.txn_read_ts = None
        self.state.py_table_snapshot = None

    def advance_checkpoint(self):
        # Fold the leader's stable into the follower via a new checkpoint.
        if self.state.ts == 0:
            return
        oldest = max(1, self.state.last_advance_checkpoint_ts)
        self.conn.set_timestamp('oldest_timestamp=%s,stable_timestamp=%s'
                                % (self.timestamp_str(oldest), self.timestamp_str(self.state.ts)))
        self.state.oldest_ts = oldest
        self.state.last_advance_checkpoint_ts = self.state.ts
        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

    def force_evict(self, node):
        # Force-evict the follower's ingest leaf so checkpointed keys fall through to stable on later
        # reads: reconciliation drops entries below the prune timestamp (already in stable) and keeps
        # fresher ones. This is the production lifecycle that drives the follower to read from stable.
        ingest_uri = 'file:' + node.dsc_uri[len('layered:'):] + '.wt_ingest'
        evict_cursor = node.session.open_cursor(ingest_uri, None, 'debug=(release_evict)')
        try:
            for key in list(self.state.py_table):
                evict_cursor.set_key(key)
                if evict_cursor.search() == 0:
                    evict_cursor.reset()
        finally:
            evict_cursor.close()

    # --- read application + comparison -----------------------------------

    def _read(self, nodes, trace, do):
        # The per-op check: run do(cursor) on the layered then reference cursor of both nodes and
        # compare (ret, key, value), then track cur_pos for a later positional write.
        def op_result(cursor):
            ret = do(cursor)
            if ret == wiredtiger.WT_NOTFOUND:
                return (ret, None, None)
            return (0, cursor.get_key(), cursor.get_value())

        for n in nodes:
            ret_dsc = op_result(n.dsc_c)
            ret_asc = op_result(n.asc_c)
            if ret_dsc != ret_asc:
                self.report_mismatch(n, ret_dsc, ret_asc, trace, 'read result differs')
            ret, key, _ = ret_dsc
            self.state.cur_pos = key if ret == 0 else None

    def _search_near_ceiling(self, cursor, key, exact_expected):
        # Canonicalize search_near to the CEILING (smallest key >= key); if it lands below key, step to
        # the next. For an absent key the layered and plain cursors deterministically prefer OPPOSITE
        # neighbors (layered floor, plain ceiling) -- both contract-legal -- so the ceiling step makes
        # them comparable. When the key is present it must be found exactly: assert cmp == 0 so the
        # ceiling step can't paper over a layered cursor that skipped a live exact match.
        cursor.set_key(key)
        cmp = cursor.search_near()
        if cmp == wiredtiger.WT_NOTFOUND:
            return wiredtiger.WT_NOTFOUND
        if exact_expected:
            self.assertEqual(cmp, 0, 'search_near missed an existing key %r (cmp=%d, trace %s)'
                             % (key, cmp, self.trace.path))
            return 0
        return cursor.next() if cmp < 0 else 0

    def report_mismatch(self, node, ret_dsc, ret_asc, trace, reason):
        # The failing op is the last line written to the trace file.
        role = 'leader' if node.conn == self.conn else 'follower'
        self.fail('\n'.join([
            'layered-vs-reference mismatch on %s node: %s' % (role, reason),
            'layered:   %r' % (ret_dsc,),
            'reference: %r' % (ret_asc,),
            'trace file: %s' % trace.path]))

    def pick_key(self, rnd, w):
        # Existing key vs missing key, weighted by the given existing/missing config.
        if self.state.py_table and rnd.choices((True, False), weights=(w.existing, w.missing))[0]:
            return rnd.choice(list(self.state.py_table))

        # pool step is 10, so if every pool key is live, pool_key + 5 is guaranteed absent.
        absent = [k for k in self.pool if k not in self.state.py_table]
        return rnd.choice(absent) if absent else rnd.choice(self.pool) + 5

    # --- verification ----------------------------------------------------

    def _scan_cursor(self, cursor):
        cursor.reset()
        out = []
        while cursor.next() != wiredtiger.WT_NOTFOUND:
            out.append((cursor.get_key(), cursor.get_value()))
        return out

    def full_scan(self, nodes, trace):
        # Whole-table cross-check: each node's layered scan == its reference, and leader == follower.
        per_node = []
        for n in nodes:
            dsc = self._scan_cursor(n.dsc_c)
            asc = self._scan_cursor(n.asc_c)
            if dsc != asc:
                self.fail('full-scan layered != reference (trace %s)\nlayered=%r\nref=%r'
                          % (trace.path, dsc, asc))
            per_node.append(dsc)

        # full_scan also cross-checks the leader and follower layered views against each other.
        if per_node[0] != per_node[1]:
            self.fail('leader layered scan != follower layered scan (trace %s)' % trace.path)

    # --- operations -------------------------------------------------------
    # Each op is self-contained: it generates its own argument, traces itself, does the work, and
    # updates the model.

    def op_next(self, nodes, rnd, trace):
        trace.log('next')
        self._read(nodes, trace, lambda c: c.next())

    def op_prev(self, nodes, rnd, trace):
        trace.log('prev')
        self._read(nodes, trace, lambda c: c.prev())

    def op_search(self, nodes, rnd, trace):
        key = self.pick_key(rnd, self.weights.search_key)
        trace.log('search %r' % key)
        self._read(nodes, trace, lambda c: (c.set_key(key), c.search())[1])

    def op_search_near(self, nodes, rnd, trace):
        key = self.pick_key(rnd, self.weights.search_key)
        # A present key must be found exactly -- but only when the read sees latest. In an as-of-past
        # txn py_table (latest) may include keys not visible at read_ts, so don't expect an exact match.
        exact = key in self.state.py_table and self.state.txn_read_ts is None
        trace.log('search_near %r' % key)
        self._read(nodes, trace, lambda c: self._search_near_ceiling(c, key, exact))

    def op_reset(self, nodes, rnd, trace):
        trace.log('reset')
        for n in nodes:
            n.reset_all()
        self.state.cur_pos = None

    def op_put(self, nodes, rnd, trace):
        key = rnd.choice(self.pool)
        trace.log('put %r' % key)
        value = self.new_value(key)
        self._write_txn(nodes, lambda c: (c.set_key(key), c.set_value(value), c.insert())[-1], 'put')
        self.state.py_table[key] = value
        self.state.cur_pos = None

    def op_remove(self, nodes, rnd, trace):
        # An existing key (a real delete) or a missing one (layered and reference must agree).
        key = self.pick_key(rnd, self.weights.remove_key)
        trace.log('remove %r' % key)
        self._write_txn(nodes, lambda c: (c.set_key(key), c.remove())[-1], 'remove')
        self.state.py_table.pop(key, None)
        self.state.cur_pos = None

    def op_pos_update(self, nodes, rnd, trace):
        # Positional write: keeps the cursor on cur_pos.
        key = self.state.cur_pos
        trace.log('pos_update %r' % key)
        value = self.new_value(key)
        if self._positional(nodes, lambda c: (c.set_value(value), c.update())[-1], 'pos_update'):
            self.state.py_table[key] = value   # only record the key if the update actually hit

    def op_pos_remove(self, nodes, rnd, trace):
        # Removes the current key.
        key = self.state.cur_pos
        already_removed = key not in self.state.py_table   # a repeat remove of an already-deleted key
        trace.log('pos_remove %r' % key)
        self._positional(nodes, lambda c: c.remove(), 'pos_remove')
        self.state.py_table.pop(key, None)
        # FIXME-WT-17827: removing an already-removed key returns WT_NOTFOUND but does not clean up the
        # follower layered cursor's position (a plain cursor resets), so a later iterate would diverge.
        # Reset just the layered (dsc) cursor to realign it with the reference. Remove once WT-17827 lands.
        if already_removed:
            for n in nodes:
                n.dsc_c.reset()
            self.state.cur_pos = None

    def op_txn_begin(self, nodes, rnd, trace):
        # No txn open -> begin one (flavor by the txn_mode weights); a txn open -> end it.
        txn_weights = self.weights.txn_mode

        # Close the txn if one is running
        if self.state.txn is not Txn.NO:
            if rnd.choices((True, False), weights=(txn_weights.commit, txn_weights.rollback))[0]:
                trace.log('commit')
                self.commit_txn(nodes)
            else:
                trace.log('rollback')
                self.rollback_txn(nodes)
            return

        mode = rnd.choices(
            [Txn.SNAPSHOT, Txn.READ_COMMITTED, Txn.READ_UNCOMMITTED, Txn.READ_TIMESTAMP],
            weights=[txn_weights.snapshot, txn_weights.read_committed, txn_weights.read_uncommitted, txn_weights.read_timestamp],
            k=1)[0]

        # A read_timestamp just needs to be valid: in [oldest_ts, ts] with oldest_ts >= 1.
        read_ts = None
        if mode is Txn.READ_TIMESTAMP:
            if self.state.ts >= self.state.oldest_ts >= 1:
                read_ts = rnd.randint(self.state.oldest_ts, self.state.ts)
            else:
                mode = Txn.SNAPSHOT
        trace.log('txn_begin %r' % ((read_ts, mode.value),))

        # Build the config
        cfg_parts = []
        if mode in (Txn.READ_COMMITTED, Txn.READ_UNCOMMITTED):
            cfg_parts.append('isolation=' + mode.value)
        if read_ts is not None:
            cfg_parts.append('read_timestamp=' + self.timestamp_str(read_ts))
        for n in nodes:
            n.session.begin_transaction(','.join(cfg_parts))

        # Update state
        self.state.txn = mode
        self.state.txn_wrote = False
        self.state.txn_read_ts = read_ts
        self.state.py_table_snapshot = dict(self.state.py_table)

        # Coverage counters.
        if mode is Txn.READ_TIMESTAMP:
            self.state.n_read_ts += 1
        elif mode is Txn.READ_COMMITTED:
            self.state.n_iso_rc += 1
        elif mode is Txn.READ_UNCOMMITTED:
            self.state.n_iso_ru += 1

    def _checkpoint(self, nodes):
        # Release any pinned snapshot (reset cursors), advance the checkpoint, clear position.
        for n in nodes:
            n.reset_all()
        self.advance_checkpoint()
        self.state.cur_pos = None

    def scen_advance_checkpoint(self, nodes, rnd, trace):
        trace.log('advance_checkpoint')
        self.state.n_advance += 1
        self._checkpoint(nodes)

    def scen_evict(self, nodes, rnd, trace):
        # Checkpoint (resets cursors, releasing pins) THEN drain the follower ingest so later reads
        # fall through to stable. The checkpoint is required to drain everything -- eviction only
        # prunes ingest entries already in stable, so without it the drain might be restricted to evict
        # a big part of the content; and a cursor pinning the ingest leaf blocks eviction, so the reset
        # (in _checkpoint) comes first.
        trace.log('evict')
        self.state.n_evict += 1
        self._checkpoint(nodes)
        follower = nodes[1]
        self.force_evict(follower)

    def scen_full_scan(self, nodes, rnd, trace):
        # full scan of the table, position-breaking.
        trace.log('full_scan')
        self.full_scan(nodes, trace)
        self.state.n_full_scan += 1
        self.state.cur_pos = None

    def scen_bulk_insert(self, nodes, rnd, trace):
        # Insert a batch (~1/10 of the pool) at random keys to grow the tables fast, through the SAME
        # cursors the chain uses (so the insert resets the cursor we operate on). Deliberately NOT
        # checkpointed, so the new entries stay in the follower ingest.
        batch = [(k, self.new_value(k))
                 for k in rnd.sample(self.pool, max(1, round(len(self.pool) * self.weights.bulk_insert_frac)))]
        trace.log('bulk_insert %r' % ([k for k, _ in batch],))

        def do_node(node):
            for k, v in batch:
                rd = (node.dsc_c.set_key(k), node.dsc_c.set_value(v), node.dsc_c.insert())[-1]
                ra = (node.asc_c.set_key(k), node.asc_c.set_value(v), node.asc_c.insert())[-1]
                self.assertEqual(rd, ra, 'bulk_insert result differs at key=%r layered=%r reference=%r'
                                 ' (trace %s)' % (k, rd, ra, self.trace.path))
        self._txn_scope(nodes, do_node)
        for k, v in batch:
            self.state.py_table[k] = v
        self.state.cur_pos = None   # insert leaves the cursor unpositioned
        self.state.n_bulk_insert += 1

    def _bulk_remove_keys(self, nodes, victims):
        # Per-key remove of the victims through the chain cursors.
        def do_node(node):
            for k in victims:
                rd = (node.dsc_c.set_key(k), node.dsc_c.remove())[-1]
                ra = (node.asc_c.set_key(k), node.asc_c.remove())[-1]
                self.assertEqual(rd, ra, 'bulk_remove result differs at key=%r layered=%r reference=%r'
                                 ' (trace %s)' % (k, rd, ra, self.trace.path))
        self._txn_scope(nodes, do_node)

    def _truncate_range(self, nodes, lo, hi):
        # Range truncate [lo, hi] on the layered + reference tables via dedicated bound cursors.
        def do_node(node):
            for uri in (node.dsc_uri, node.asc_uri):
                lo_c = node.session.open_cursor(uri)
                hi_c = node.session.open_cursor(uri)
                lo_c.set_key(lo); hi_c.set_key(hi)
                node.session.truncate(None, lo_c, hi_c, None)
                lo_c.close(); hi_c.close()
        self._txn_scope(nodes, do_node)

    def scen_bulk_remove(self, nodes, rnd, trace):
        # Delete a large fraction (40/80/100%) of the file at once -- a big-delete merge on the
        # follower -- by per-key remove or one range truncate, through the chain cursors.

        live = sorted(self.state.py_table)
        if not live:
            return # Nothing to remove

        # Choose the victims.
        frac = rnd.choice((0.4, 0.8, 1.0))
        n = max(1, round(frac * len(live)))
        start = rnd.randint(0, len(live) - n)
        victims = live[start:start + n]

        # Choose the mode.
        bw = self.weights.bulk_remove_mode
        use_truncate = rnd.choices((False, True), weights=(bw.remove, bw.truncate))[0]
        trace.log('bulk_%s frac=%s n=%d' % ('truncate' if use_truncate else 'remove', frac, n))

        if use_truncate:
            self._truncate_range(nodes, victims[0], victims[-1])
        else:
            self._bulk_remove_keys(nodes, victims)

        # Update the state.
        for k in victims:
            del self.state.py_table[k]
        self.state.cur_pos = None
        self.state.n_bulk_remove += 1

    # --- operation generation -------------------------------------------

    def _legal(self, op):
        # Which ops are legal in the current transaction context / position.

        txn = self.state.txn
        if op.needs_position and self.state.cur_pos is None:
            return False
        if txn is Txn.NO:
            return op.no_txn
        if not op.in_txn:          # advance_checkpoint / evict not allowed inside a txn
            return False
        if not write_allowed(txn):
            return not op.is_write # no writes in a read-only transaction
        return True

    def run_op(self, nodes, rnd, trace):
        # Pick one legal op by its weight and run it. Each op_* method does its own arg generation,
        # tracing, and state update.
        cands = [op for op in self.ops if self._legal(op)]
        op = rnd.choices(cands, weights=[op.weight for op in cands], k=1)[0]
        op.fn(nodes, rnd, trace)
        self.record_metrics(op)

    # --- metrics / tracing -----------------------------------------------

    def record_metrics(self, op):
        # Instrumentation (not a product check): op-diversity, positioned-run length, and rising-edge
        # counts of the table reaching a full / empty pool.
        s = self.state
        s.op_counts[op.fn.__name__] = s.op_counts.get(op.fn.__name__, 0) + 1
        if s.cur_pos is not None:
            s.chain_run += 1
        elif s.chain_run:
            s.chain_total += s.chain_run; s.chain_count += 1; s.chain_run = 0
        n = len(s.py_table)
        if n > s.max_n: s.max_n = n
        full, empty = n >= len(self.pool), n == 0
        if full and not s.prev_full:
            s.n_reached_full += 1
        if empty and not s.prev_empty:
            s.n_reached_empty += 1
        s.prev_full, s.prev_empty = full, empty

    def metrics(self):
        # Aggregate run shape (instrumentation + workload-diversity, not a product check).
        s = self.state
        chain_avg = (s.chain_total / s.chain_count) if s.chain_count else 0.0
        stable, ingest = self.follower_read_split()
        stable_frac = (stable / (stable + ingest)) if (stable + ingest) else 0.0
        total = sum(s.op_counts.values()) or 1
        shares = {k: v / total for k, v in s.op_counts.items()}
        top_share = max(shares.values()) if shares else 1.0
        # Effective number of distinct ops = exp(Shannon entropy): ~1 when one op dominates, larger
        # when the stream is diverse. Guards against a degenerate single-op workload.
        eff_ops = math.exp(-sum(p * math.log(p) for p in shares.values() if p > 0)) if shares else 0.0
        return dict(chain_avg=chain_avg, chain_count=s.chain_count, n_reached_full=s.n_reached_full,
                    n_reached_empty=s.n_reached_empty, stable_frac=stable_frac, stable=stable,
                    ingest=ingest, top_share=top_share, eff_ops=eff_ops, shares=shares)

    def follower_read_split(self):
        # Follower layered reads served from stable vs ingest. Uses only next/prev/search; the search_near
        # stable counter is impure (counts the current_cursor==NULL case, FIXME-WT-15545).
        stat_cursor = self.session_follow.open_cursor('statistics:')
        try:
            get_stat = lambda name: stat_cursor[getattr(wiredtiger.stat.conn, name)][2]
            stable = get_stat('layered_curs_next_stable') + get_stat('layered_curs_prev_stable') + \
                get_stat('layered_curs_search_stable')
            ingest = get_stat('layered_curs_next_ingest') + get_stat('layered_curs_prev_ingest') + \
                get_stat('layered_curs_search_ingest')
            return stable, ingest
        finally:
            stat_cursor.close()

    def log_metrics(self):
        m = self.metrics()
        self.pr('METRICS: chain_avg=%.1f chain_count=%d n_reached_full=%d n_reached_empty=%d '
                'stable_frac=%.3f stable=%d ingest=%d eff_ops=%.1f top_share=%.1f%%'
                % (m['chain_avg'], m['chain_count'], m['n_reached_full'], m['n_reached_empty'],
                   m['stable_frac'], m['stable'], m['ingest'], m['eff_ops'], 100 * m['top_share']))
        s = self.state
        self.pr('DIAGNOSTIC: max_n=%d pool=%d n_advance=%d n_evict=%d n_bulk_insert=%d n_bulk_remove=%d n_full_scan=%d'
                % (s.max_n, len(self.pool), s.n_advance, s.n_evict, s.n_bulk_insert, s.n_bulk_remove, s.n_full_scan))
        dist = ' '.join('%s=%.1f%%' % (k.replace('op_', '').replace('scen_', ''), 100 * m['shares'][k])
                        for k in sorted(m['shares'], key=m['shares'].get, reverse=True))
        self.pr('DIAGNOSTIC: diversity | %s' % dist)
        return m

    def assert_workload_coverage(self):
        # Coverage guard (NOT a product assertion): a failure means the TEST stopped covering a
        # dimension, not that the product is wrong. The per-op layered-vs-reference comparison is what
        # catches product bugs; these floors keep a profile from silently degenerating.
        m = self.metrics()
        s = self.state

        # The merge of two non-empty constituents: the follower must read from stable a real fraction
        # of the time, or it is effectively an ingest-only test. Every tuned profile clears this easily.
        self.assertGreater(m['stable'] + m['ingest'], 0, 'no follower layered reads at all')
        self.assertGreaterEqual(m['stable_frac'], 0.15,
            'follower read from stable too rarely (%.3f) -- merge not exercised' % m['stable_frac'])

        # Both extremes of table size: a full pool and an emptied pool.
        self.assertGreater(m['n_reached_full'], 0, 'pool never filled -- full-table merge not exercised')
        self.assertGreater(m['n_reached_empty'], 0, 'pool never emptied -- empty-table path not exercised')

        # Diversity: no single op may dominate the stream (the profiles are deliberately balanced).
        self.assertGreaterEqual(m['eff_ops'], 5.0, 'workload not diverse enough (eff_ops=%.1f)' % m['eff_ops'])
        self.assertLessEqual(m['top_share'], 0.45,
            'one op dominates the workload (top_share=%.1f%%)' % (100 * m['top_share']))

        # Positioned chains form, and the txn / scenario surface ran.
        self.assertGreaterEqual(m['chain_avg'], 2.0, 'positioned chains too short (chain_avg=%.1f)' % m['chain_avg'])
        self.assertGreater(s.n_positional, 0, 'no positional update/remove ops were exercised')
        self.assertGreater(s.n_read_ts, 0, 'no read_timestamp (as-of-past) txns were exercised')
        self.assertGreater(s.n_iso_rc, 0, 'no read-committed txns were exercised')
        self.assertGreater(s.n_iso_ru, 0, 'no read-uncommitted txns were exercised')
        self.assertGreater(s.n_full_scan, 0, 'no full_scan ops were exercised')
        self.assertGreater(s.n_bulk_insert, 0, 'no bulk_insert scenarios were exercised')
        self.assertGreater(s.n_bulk_remove, 0, 'no bulk_remove scenarios were exercised')

    def open_trace(self, seed, tag):
        path = os.path.join(os.getcwd(), 'stress_trace_%s_%d.txt' % (tag, seed))
        self.pr('SEED=%d trace=%s' % (seed, path))
        return EventTrace(path, 'test_layered_cursor_stress seed=%d tag=%s' % (seed, tag))

    # --- driver ----------------------------------------------------------

    def run_sequence(self, seed, tag, n_ops):
        rnd = random.Random(seed)
        trace = self.open_trace(seed, tag)
        self.trace = trace   # for failure messages raised deep in the write path
        nodes = self.make_nodes(tag)
        try:
            for _ in range(n_ops):
                self.run_op(nodes, rnd, trace)
            if self.state.txn is not Txn.NO:
                self.commit_txn(nodes) # close any txn left open before verifying
            # Scan the tables as a final verification.
            self.full_scan(nodes, trace)
            if self.state.chain_run:   # flush the final positioned run into the average
                self.state.chain_total += self.state.chain_run
                self.state.chain_count += 1
                self.state.chain_run = 0
            self.log_metrics()         # running aggregate over all seeds so far
        finally:
            # A txn left open by a mid-sequence failure rolls back when the connection closes at
            # teardown (these are never prepared), so no explicit rollback here.
            for n in nodes:
                n.close()
            trace.close()

    # --- tests -----------------------------------------------------------

    def test_smoke(self):
        # Short seeded run with writes, starting from empty tables. The profile dimension crosses every
        # test method, so run the smoke once (on the first profile) rather than once per profile.
        if self.profile != next(iter(WORKLOAD_PROFILES)):
            self.skipTest('smoke is profile-independent; runs once')
        self.setup_connections(Weights())
        self.run_sequence(seed=12345, tag='smoke', n_ops=80)

    def test_workload(self):
        # One balanced workload profile (selected by the scenario) over a few seeds, fresh tables per
        # seed, starting empty. Coverage is asserted on the cumulative shape across the seeds.
        prof = WORKLOAD_PROFILES[self.profile]
        self.setup_connections(prof['weights'], n_keys=prof['n_keys'])
        for seed in range(prof['seeds']):
            self.run_sequence(seed=seed, tag='%s%d' % (self.profile, seed), n_ops=prof['n_ops'])
        self.assert_workload_coverage()
