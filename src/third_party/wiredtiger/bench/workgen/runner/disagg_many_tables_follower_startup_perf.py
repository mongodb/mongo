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
# disagg_many_tables_follower_startup_perf.py
#   Disagg follower startup performance benchmark for the many-table case.
#   Measures how long it takes a fresh follower to (a) open and (b) ingest a
#   checkpoint produced by a leader that just created N layered tables.
#
#     Phase 1 (leader):    create N layered tables, light timestamped populate,
#                          checkpoint, capture checkpoint_meta from PALI, close.
#     Phase 2 (follower):  open the same home as a follower (no checkpoint_meta
#                          on the open call) timed in isolation, then drive
#                          the pickup separately via reconfigure(checkpoint_meta=),
#                          also timed.
#
#   Both phases run against the same WT home directory and the same PALI store
#   (kv_home/, auto-created by palite under <home>/). When the follower opens
#   after the leader has closed, disaggregated.local_files_action=delete (the
#   default) wipes the leader's leftover .wt / .wt_ingest files while kv_home/
#   stays put where the checkpoint we're picking up actually lives.
#
#   --skip-setup skips phase 1 (use when a leader has already populated <home>
#   and checkpointed). Phase 2 opens the follower, obtains checkpoint_meta via
#   pl_get_complete_checkpoint on the follower (PALI under <home>/kv_home/ from
#   the same leader run), runs pickup, and counts tables via the metadata cursor
#   (no --num-tables). Mutually exclusive with --num-tables. If kv_home/ is
#   missing or empty, follower open creates a fresh PALI store and lookup returns
#   WT_NOTFOUND; copy the leader's entire kv_home/ under the home before skip-setup.
#
#   Env: WT_BUILDDIR must point at the build dir containing
#        ext/page_log/palite/libwiredtiger_palite.so.
#

from runner import *
from wiredtiger import *
from workgen import *
import os, time

# ----------------------------------------------------------------------
# Configuration
# ----------------------------------------------------------------------
KEYS_PER_TABLE = 5            # tiny populate for phase 3 (currently commented out)

PAGE_LOG = "palite"
TABLE_PREFIX = "test_disagg_pickup_"

# ----------------------------------------------------------------------
# Set up home. We use a single WT home for both the leader and the follower:
# the leader writes local files there alongside palite's kv_home/ (the PALI
# backing store). When the follower opens the same dir after the leader has
# closed, disaggregated.local_files_action=delete (the default) wipes the
# leftover .wt / .wt_ingest files, while palite's kv_home/ stays put  that's
# where the checkpoint we're picking up actually lives.
# ----------------------------------------------------------------------
context = Context()
_setup_group = context.parser.add_mutually_exclusive_group()
_setup_group.add_argument("--num-tables", dest="num_tables", type=int, default=None,
    help="Number of layered tables the leader creates (default: 10000; not allowed with --skip-setup)")
_setup_group.add_argument("--skip-setup", dest="skip_setup", action="store_true",
    help="Skip phase 1; run phase 2 only against an existing home. Table count is derived from metadata after pickup.")
# Preserve the existing home (kv_home/ and WT files) when skipping phase 1.
# --keep must be in sys.argv before initialize() calls parse_args() and rmtree.
if "--skip-setup" in sys.argv and "--keep" not in sys.argv:
    sys.argv.append("--keep")
context.initialize()           # parses all args, creates args.home
home = context.args.home

SKIP_SETUP = context.args.skip_setup
if SKIP_SETUP:
    NUM_TABLES = None
else:
    NUM_TABLES = context.args.num_tables if context.args.num_tables is not None else 10000

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
    # tight create loop.
    f"file_manager=(close_handle_minimum=10,close_idle_time=2,close_scan_interval=1),"
    f'extensions=("{ext_path}"=(config="(verbose=0)")),'
    f"disaggregated=(page_log={PAGE_LOG},lose_all_my_data=true,"
)


def fetch_checkpoint_meta(conn):
    # Latest complete-checkpoint blob from PALI via the page log.
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
            "Follower open with an empty or missing kv_home creates a new empty PALI store. "
            "Copy the leader's entire kv_home/ under this home (or run phase 1 here first). "
            f"Expected PALI DB roughly at: {kv}"
        ) from ex
    finally:
        page_log.terminate(meta_session)
        meta_session.close()
    assert ckpt_meta, "no complete checkpoint metadata returned from PALI"
    print(f"  checkpoint_meta length: {len(ckpt_meta)} bytes")
    return ckpt_meta


def count_tables_uri_prefix(conn, table_uri_prefix):
    # Count metadata: keys starting with table_uri_prefix.
    session = conn.open_session()
    md = session.open_cursor("metadata:", None, None)
    n = 0
    for key, _ in md:
        if key.startswith(table_uri_prefix):
            n += 1
    md.close()
    session.close()
    return n


# ----------------------------------------------------------------------
# Phase 1: leader creates N layered tables, populates a few keys, checkpoints.
# ----------------------------------------------------------------------
if SKIP_SETUP:
    print("=" * 70)
    print("Phase 1 skipped (--skip-setup; using existing home and PALI checkpoint)")
    print("=" * 70)
else:
    print("=" * 70)
    print(f"Phase 1: leader creating {NUM_TABLES} layered tables")
    print("=" * 70)

    leader_conn = wiredtiger_open(
        home, "create," + base_conn_config + 'role="leader")')
    leader_session = leader_conn.open_session()

    # Initialize timestamps before any writes so commits can use commit_timestamp.
    leader_conn.set_timestamp("stable_timestamp=1")

    table_cfg = "key_format=S,value_format=S,type=layered,block_manager=disagg"

    t0 = time.time()
    for i in range(NUM_TABLES):
        uri = f"table:{TABLE_PREFIX}{i}"
        leader_session.create(uri, table_cfg)
        if (i + 1) % 10000 == 0:
            print(f"  created {i+1}/{NUM_TABLES}  ({time.time()-t0:.1f}s)")
            # Checkpoint allows dhandle memory to be released.
            leader_session.checkpoint()
    print(f"  all {NUM_TABLES} tables created in {time.time()-t0:.1f}s")

    print("  taking checkpoint")
    t0 = time.time()
    leader_session.checkpoint()
    print(f"  checkpoint completed in {time.time()-t0:.1f}s")

    ckpt_meta = fetch_checkpoint_meta(leader_conn)

    leader_conn.close()
    print("  leader closed")

# ----------------------------------------------------------------------
# Phase 2: times follower open, and reconfigure with checkpoint_meta.
# ----------------------------------------------------------------------
print("=" * 70)
print("Phase 2: follower picking up the checkpoint")
print("=" * 70)

# Open the follower WITHOUT checkpoint_meta first, so the open call does no
# pickup work  this isolates pure connection-open cost.
follower_open_config = (
    "create," + base_conn_config + 'role="follower")'
)

print("  opening follower connection (no pickup yet, timed)")
open_t0 = time.time()
follower_conn = wiredtiger_open(home, follower_open_config)
open_elapsed = time.time() - open_t0
print(f"  WIREDTIGER_OPEN (no pickup) took {open_elapsed:.2f}s")
# Machine-readable line for the evergreen perf parser.
print(f"PERF wiredtiger_open_no_pickup_secs: {open_elapsed:.4f}")

if SKIP_SETUP:
    ckpt_meta = fetch_checkpoint_meta(follower_conn)

# Now drive the checkpoint pickup via reconfigure. This matches MongoDB's
# real usage (control plane hands the follower a checkpoint_meta after open)
# and lets us time pickup separately from open.
print("  reconfiguring with checkpoint_meta (pickup, timed)")
pickup_t0 = time.time()
follower_conn.reconfigure(f'disaggregated=(checkpoint_meta="{ckpt_meta}")')
pickup_elapsed = time.time() - pickup_t0
print(f"  RECONFIGURE (pickup) took {pickup_elapsed:.2f}s")
# Machine-readable line for the evergreen perf parser.
print(f"PERF reconfigure_pickup_secs: {pickup_elapsed:.4f}")

if SKIP_SETUP:
    table_uri_prefix = f"table:{TABLE_PREFIX}"
    NUM_TABLES = count_tables_uri_prefix(follower_conn, table_uri_prefix)
    print(f"  derived num_tables from metadata: {NUM_TABLES} ({table_uri_prefix}*)")

follower_conn.close()
print("  follower closed")

print("=" * 70)
print("SUMMARY")
print("=" * 70)
print(f"  num_tables                    = {NUM_TABLES}")
print(f"  follower wiredtiger_open      = {open_elapsed:.2f}s   (no pickup)")
print(f"  follower reconfigure pickup   = {pickup_elapsed:.2f}s")
# print(f"  follower read all tables      = {read_elapsed:.2f}s")
print(f"  artifacts under               = {home}")
