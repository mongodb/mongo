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
# disagg_many_tables_follower_update_perf.py
#   Disagg follower pick-up performance for the many-table *update* path.
#   Measures how long it takes a follower that already owns N layered tables
#   to pick up a new checkpoint after the leader has dirtied those tables'
#   shared file: checkpoint metadata (the __disagg_update_file_meta path
#   inside __disagg_apply_checkpoint_meta).
#
#     Phase 1 (leader):     create N layered tables, checkpoint (meta1).
#     Phase 2 (follower):   open, pick up meta1 (setup; establishes local
#                           metadata for all N tables). Unmeasured primary.
#     Phase 3 (repeated):   leader mutates every table, checkpoints; follower
#                           timed reconfigure(checkpoint_meta=). Repeat for
#                           --num-pickups samples (after --warmup-pickups),
#                           then report the average wall-clock pick-up and
#                           disagg_apply_checkpoint_meta_time (both ms).
#
#   Leader and follower use separate WT homes that share one PALI store:
#     <home>/                 leader
#       kv_home/
#     <home>/follower/        follower
#       kv_home -> ../kv_home
#
#   Env: WT_BUILDDIR must point at the build dir containing
#        ext/page_log/palite/libwiredtiger_palite.so.
#

from runner import *
from wiredtiger import *
from wiredtiger import stat
from workgen import *
import os, time

# ----------------------------------------------------------------------
# Configuration
# ----------------------------------------------------------------------
PAGE_LOG = "palite"
TABLE_PREFIX = "test_disagg_update_pickup_"
TABLE_CFG = "key_format=S,value_format=S,type=layered,block_manager=disagg"

context = Context()
context.parser.add_argument("--num-tables", dest="num_tables", type=int, default=10000,
    help="Number of layered tables (default: 10000)")
context.parser.add_argument("--num-pickups", dest="num_pickups", type=int, default=5,
    help="Timed update pick-ups to average (default: 5)")
context.parser.add_argument("--warmup-pickups", dest="warmup_pickups", type=int, default=1,
    help="Discard this many leading pick-ups before averaging (default: 1)")
context.initialize()
home = context.args.home
NUM_TABLES = context.args.num_tables
NUM_PICKUPS = context.args.num_pickups
WARMUP_PICKUPS = context.args.warmup_pickups
if NUM_PICKUPS < 1:
    raise RuntimeError("--num-pickups must be >= 1")
if WARMUP_PICKUPS < 0:
    raise RuntimeError("--warmup-pickups must be >= 0")

follower_home = os.path.join(home, "follower")
os.mkdir(follower_home)
# Pre-create the shared PALI store and symlink it into the follower home
# (same layout as helper_disagg.early_setup).
os.mkdir(os.path.join(home, "kv_home"))
os.symlink("../kv_home", os.path.join(follower_home, "kv_home"), target_is_directory=True)

wt_builddir = os.environ.get("WT_BUILDDIR")
if not wt_builddir:
    raise RuntimeError("WT_BUILDDIR must be set (path to the build dir)")
ext_path = os.path.join(wt_builddir, "ext", "page_log", PAGE_LOG,
                        "libwiredtiger_" + PAGE_LOG + ".so")
if not os.path.isfile(ext_path):
    raise RuntimeError("page_log extension not found at " + ext_path)

base_conn_config = (
    f"statistics=(all),statistics_log=(wait=1,on_close,json=true),"
    f"cache_size=20GB,precise_checkpoint=true,"
    # Aggressive sweep: scan every 1s, expire dhandles after 2s of idleness,
    # don't keep a floor of 250 open. Keeps fd usage bounded under our
    # tight create / mutate loops.
    f"file_manager=(close_handle_minimum=10,close_idle_time=2,close_scan_interval=1),"
    f'extensions=("{ext_path}"=(config="(verbose=0)")),'
    f"disaggregated=(page_log={PAGE_LOG},lose_all_my_data=true,"
)


def fetch_checkpoint_meta(conn):
    print("  fetching checkpoint_meta from PALI")
    page_log = conn.get_page_log(PAGE_LOG)
    meta_session = conn.open_session()
    try:
        (_, _, _, ckpt_meta) = page_log.pl_get_complete_checkpoint(meta_session)
    except Exception as ex:
        if "WT_NOTFOUND" not in str(ex):
            raise
        kv = os.path.join(home, "kv_home", "checkpoints.db")
        raise RuntimeError(
            "pl_get_complete_checkpoint: WT_NOTFOUND (no completed checkpoint in PALI). "
            f"Expected PALI DB roughly at: {kv}"
        ) from ex
    finally:
        page_log.terminate(meta_session)
        meta_session.close()
    assert ckpt_meta, "no complete checkpoint metadata returned from PALI"
    print(f"  checkpoint_meta length: {len(ckpt_meta)} bytes")
    return ckpt_meta


def get_conn_stat(conn, stat_field):
    session = conn.open_session()
    try:
        c = session.open_cursor("statistics:", None, None)
        try:
            return c[stat_field][2]
        finally:
            c.close()
    finally:
        session.close()


def mutate_all_tables(session, commit_ts, value):
    t0 = time.time()
    for i in range(NUM_TABLES):
        uri = f"table:{TABLE_PREFIX}{i}"
        session.begin_transaction()
        c = session.open_cursor(uri)
        c["k"] = value
        c.close()
        session.commit_transaction(f"commit_timestamp={commit_ts}")
        if (i + 1) % 10000 == 0:
            print(f"  mutated {i+1}/{NUM_TABLES}  ({time.time()-t0:.1f}s)")
    print(f"  all {NUM_TABLES} tables mutated in {time.time()-t0:.1f}s")


# ----------------------------------------------------------------------
# Phase 1: leader creates N layered tables and checkpoints.
# ----------------------------------------------------------------------
print("=" * 70)
print(f"Phase 1: leader creating {NUM_TABLES} layered tables")
print("=" * 70)

leader_conn = wiredtiger_open(
    home, "create," + base_conn_config + 'role="leader")')
leader_session = leader_conn.open_session()

# Initialize timestamps before any writes so commits can use commit_timestamp.
leader_conn.set_timestamp("stable_timestamp=1")

t0 = time.time()
for i in range(NUM_TABLES):
    uri = f"table:{TABLE_PREFIX}{i}"
    leader_session.create(uri, TABLE_CFG)
    if (i + 1) % 10000 == 0:
        print(f"  created {i+1}/{NUM_TABLES}  ({time.time()-t0:.1f}s)")
        # Checkpoint allows dhandle memory to be released.
        leader_session.checkpoint()
print(f"  all {NUM_TABLES} tables created in {time.time()-t0:.1f}s")

print("  taking initial checkpoint (meta1)")
t0 = time.time()
leader_session.checkpoint()
print(f"  checkpoint completed in {time.time()-t0:.1f}s")

ckpt_meta1 = fetch_checkpoint_meta(leader_conn)

# ----------------------------------------------------------------------
# Phase 2: follower picks up meta1 (setup; establishes local metadata).
# ----------------------------------------------------------------------
print("=" * 70)
print("Phase 2: follower setup pick-up (meta1, unmeasured primary)")
print("=" * 70)

follower_conn = wiredtiger_open(
    follower_home, "create," + base_conn_config + 'role="follower")')

print("  reconfiguring with checkpoint_meta (meta1)")
t0 = time.time()
follower_conn.reconfigure(f'disaggregated=(checkpoint_meta="{ckpt_meta1}")')
setup_pickup_elapsed = time.time() - t0
print(f"  setup pick-up took {setup_pickup_elapsed:.2f}s")

# ----------------------------------------------------------------------
# Phase 3: repeated mutate + checkpoint + timed follower pick-up.
# ----------------------------------------------------------------------
total_rounds = WARMUP_PICKUPS + NUM_PICKUPS
print("=" * 70)
print(f"Phase 3: {total_rounds} update pick-ups "
      f"({WARMUP_PICKUPS} warmup + {NUM_PICKUPS} measured)")
print("=" * 70)

pickup_samples_ms = []
apply_meta_samples_ms = []
commit_ts = 10
file_meta_updated_before = get_conn_stat(
    follower_conn, stat.conn.disagg_pick_up_file_meta_updated)

for round_idx in range(total_rounds):
    is_warmup = round_idx < WARMUP_PICKUPS
    label = "warmup" if is_warmup else f"sample {round_idx - WARMUP_PICKUPS + 1}/{NUM_PICKUPS}"
    print(f"----- round {round_idx + 1}/{total_rounds} ({label}) -----")

    commit_ts += 10
    mutate_all_tables(leader_session, commit_ts, f"v{round_idx}")
    leader_conn.set_timestamp(f"stable_timestamp={commit_ts}")
    print("  taking update checkpoint")
    t0 = time.time()
    leader_session.checkpoint()
    print(f"  checkpoint completed in {time.time()-t0:.1f}s")

    ckpt_meta = fetch_checkpoint_meta(leader_conn)

    print("  reconfiguring with checkpoint_meta (timed)")
    pickup_t0 = time.time()
    follower_conn.reconfigure(f'disaggregated=(checkpoint_meta="{ckpt_meta}")')
    pickup_elapsed = time.time() - pickup_t0
    pickup_ms = int(round(pickup_elapsed * 1000))
    # Stat is last-sample (SET), so read it after each pick-up.
    apply_meta_ms = get_conn_stat(follower_conn, stat.conn.disagg_apply_checkpoint_meta_time)
    print(f"  pick-up wall={pickup_ms} ms  apply_meta={apply_meta_ms} ms")

    if not is_warmup:
        pickup_samples_ms.append(pickup_ms)
        apply_meta_samples_ms.append(apply_meta_ms)

file_meta_updated = get_conn_stat(follower_conn, stat.conn.disagg_pick_up_file_meta_updated)
file_meta_delta = file_meta_updated - file_meta_updated_before
expected_updates = NUM_TABLES * total_rounds
if file_meta_delta < expected_updates:
    raise RuntimeError(
        f"Expected disagg_pick_up_file_meta_updated delta >= {expected_updates}, "
        f"got {file_meta_delta} (before={file_meta_updated_before}, after={file_meta_updated}). "
        "Shared file: checkpoint cookies likely did not change (clean trees / no-op pick-up)."
    )

avg_pickup_ms = int(round(sum(pickup_samples_ms) / len(pickup_samples_ms)))
avg_apply_meta_ms = int(round(sum(apply_meta_samples_ms) / len(apply_meta_samples_ms)))

# Single PERF lines for evergreen/atlas scraping (averaged samples only).
print(f"PERF reconfigure_update_pickup_ms: {avg_pickup_ms}")
print(f"PERF disagg_apply_checkpoint_meta_ms: {avg_apply_meta_ms}")

leader_session.close()
leader_conn.close()
follower_conn.close()

print("=" * 70)
print("SUMMARY")
print("=" * 70)
print(f"  num_tables                         = {NUM_TABLES}")
print(f"  warmup_pickups                     = {WARMUP_PICKUPS}")
print(f"  measured_pickups                   = {NUM_PICKUPS}")
print(f"  follower setup pick-up (meta1)     = {setup_pickup_elapsed:.2f}s")
print(f"  pick-up samples (ms)               = {pickup_samples_ms}")
print(f"  apply-meta samples (ms)            = {apply_meta_samples_ms}")
print(f"  avg reconfigure update pick-up     = {avg_pickup_ms} ms")
print(f"  avg disagg_apply_checkpoint_meta   = {avg_apply_meta_ms} ms")
print(f"  disagg_pick_up_file_meta_updated   = {file_meta_updated} "
      f"(delta {file_meta_delta})")
print(f"  artifacts under                    = {home}")
