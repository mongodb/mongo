/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <algorithm>
#include <atomic>
#include <catch2/catch.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "wt_internal.h"
#include "../wrappers/connection_wrapper.h"

/*
 * Before the eviction server sleeps it releases the walk points it holds. These tests call that
 * release directly with the eviction pass lock held, so the eviction server cannot run alongside
 * them, and check the state it leaves behind: no walk point, no hazard pointer in the walk session,
 * no end count on the saved walk tree, no saved walk tree, and no tree visited after the last walk
 * point. With the same lock held, the last test runs the eviction scan itself over idle trees and
 * checks that it lets a waiting writer take the handle list lock.
 */

namespace {

const int NTABLES = 10;

/*
 * Hold the eviction pass lock for the lifetime of this object, so a failed check cannot leave it
 * held: the eviction threads take it to stop, and closing the connection would then hang.
 */
struct pass_lock {
    WT_SESSION_IMPL *session;

    explicit pass_lock(WT_SESSION_IMPL *s) : session(s)
    {
        __wt_spin_lock(session, &S2C(session)->evict->evict_pass_lock);
        FLD_SET(session->lock_flags, WT_SESSION_LOCKED_PASS);
    }

    ~pass_lock()
    {
        FLD_CLR(session->lock_flags, WT_SESSION_LOCKED_PASS);
        __wt_spin_unlock(session, &S2C(session)->evict->evict_pass_lock);
    }

    pass_lock(const pass_lock &) = delete;
    pass_lock &operator=(const pass_lock &) = delete;
};

/*
 * A database directory of its own, emptied before the test and removed after the connection closes,
 * so the test's tables never meet files from an earlier run.
 */
struct test_home {
    std::string path;

    explicit test_home(const char *p) : path(p)
    {
        std::filesystem::remove_all(path);
    }

    ~test_home()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

struct walk_test {
    /* Declared before the connection so it is removed after the connection closes. */
    test_home home{"WT_TEST.evict_clear_walks"};
    connection_wrapper conn_wrapper{
      home.path, "create,statistics=(all),eviction=(evict_use_softptr=false)"};
    WT_SESSION_IMPL *session = nullptr;
    WT_CONNECTION_IMPL *conn = nullptr;
    WT_EVICT *evict = nullptr;
    std::vector<WT_CURSOR *> cursors;
    std::vector<WT_DATA_HANDLE *> dhandles;
    /* Declared last so it is released first, including when the constructor fails. */
    std::unique_ptr<pass_lock> locked;

    /*
     * The optional setup runs before the pass lock is taken, for work that needs the eviction
     * server to give up a tree, such as a bulk load.
     */
    explicit walk_test(
      int ntables = NTABLES, const std::function<void(WT_SESSION *)> &setup = nullptr)
    {
        WT_SESSION *wt_session;
        WT_CONNECTION *wt_conn = conn_wrapper.get_wt_connection();
        REQUIRE(wt_conn->open_session(wt_conn, nullptr, nullptr, &wt_session) == 0);
        session = (WT_SESSION_IMPL *)wt_session;
        conn = S2C(session);
        evict = conn->evict;
        if (setup)
            setup(wt_session);

        /* Small trees with a resident leaf page under the root, each kept open by a cursor. */
        for (int i = 0; i < ntables; ++i) {
            std::string uri = "file:test_evict_clear_walks" + std::to_string(i) + ".wt";
            WT_CURSOR *cursor;
            REQUIRE(
              wt_session->create(wt_session, uri.c_str(), "key_format=i,value_format=S") == 0);
            REQUIRE(
              wt_session->open_cursor(wt_session, uri.c_str(), nullptr, nullptr, &cursor) == 0);
            for (int k = 1; k <= 10; ++k) {
                cursor->set_key(cursor, k);
                cursor->set_value(cursor, "value");
                REQUIRE(cursor->insert(cursor) == 0);
            }
            REQUIRE(cursor->reset(cursor) == 0);
            cursors.push_back(cursor);
            dhandles.push_back(((WT_CURSOR_BTREE *)cursor)->dhandle);
        }

        /* Keep the eviction server out, as it is whenever the release runs. */
        locked.reset(new pass_lock(session));

        /* Start from a clean state in case the server left anything behind. */
        clear_all();
        REQUIRE(hazards() == 0);
        for (WT_DATA_HANDLE *dhandle : dhandles)
            ((WT_BTREE *)dhandle->handle)->evict_walk_ends = 0;
    }

    ~walk_test()
    {
        locked.reset();
        for (WT_CURSOR *cursor : cursors)
            (void)cursor->close(cursor);
    }

    /* The release runs with the handle list lock held, as the eviction server calls it. */
    void
    clear_all()
    {
        __wt_readlock(session, &conn->dhandle_lock);
        int ret = __wti_evict_clear_all_walks_and_saved_tree(session);
        __wt_readunlock(session, &conn->dhandle_lock);
        REQUIRE(ret == 0);
    }

    uint32_t
    hazards() const
    {
        return (__wt_atomic_load_uint32_relaxed(&evict->walk_session->hazards.num_active));
    }

    /* Position of a handle on the connection's handle list, counting from the head. */
    int64_t
    position(WT_DATA_HANDLE *target)
    {
        WT_DATA_HANDLE *dhandle;
        int64_t pos = 0;

        __wt_readlock(session, &conn->dhandle_lock);
        TAILQ_FOREACH (dhandle, &conn->dhqh, q) {
            if (dhandle == target)
                break;
            ++pos;
        }
        __wt_readunlock(session, &conn->dhandle_lock);
        REQUIRE(dhandle == target);
        return (pos);
    }

    /*
     * Leave a walk point in a tree the way the eviction walk does: the walk session walks to a leaf
     * page and keeps its hazard pointer, and the tree remembers the page.
     */
    void
    make_walk_point(WT_DATA_HANDLE *dhandle)
    {
        WT_SESSION_IMPL *walk_session = evict->walk_session;
        WT_BTREE *btree = (WT_BTREE *)dhandle->handle;
        WT_REF *ref = nullptr;
        int ret;

        WT_WITH_DHANDLE(walk_session, dhandle,
          ret = __wt_tree_walk(walk_session, &ref,
            WT_READ_EVICT_WALK_FLAGS | WT_READ_SKIP_INTL | WT_READ_VISIBLE_ALL));
        REQUIRE(ret == 0);
        REQUIRE(ref != nullptr);
        __wt_atomic_store_ptr_relaxed(&btree->evict_ref, ref);
    }
};

WT_BTREE *
btree_of(WT_DATA_HANDLE *dhandle)
{
    return ((WT_BTREE *)dhandle->handle);
}

} // namespace

TEST_CASE("Releasing walk points resets the end count of the saved walk tree", "[evict]")
{
    walk_test t;

    /*
     * The scan stopped on a tree it had walked to the end: the tree is the saved walk tree and has
     * an end count, but holds no walk point, so nothing makes the release visit it.
     */
    WT_DATA_HANDLE *saved = t.dhandles[NTABLES / 2];
    btree_of(saved)->evict_walk_ends = 2;
    __wti_evict_set_saved_walk_tree(t.session, saved);
    REQUIRE(btree_of(saved)->evict_ref == nullptr);
    REQUIRE(t.hazards() == 0);

    t.clear_all();

    CHECK(btree_of(saved)->evict_walk_ends == 0);
    CHECK(t.evict->walk_tree == nullptr);
}

TEST_CASE("Releasing walk points stops after the last walk point", "[evict]")
{
    walk_test t;

    /*
     * Walk points in three trees; the release has to reach the one furthest down the list. Newer
     * handles go to the head of the list, so the first table created lies beyond all of them.
     */
    std::vector<WT_DATA_HANDLE *> walked = {
      t.dhandles[NTABLES - 1], t.dhandles[NTABLES - 4], t.dhandles[NTABLES - 7]};
    int64_t last = 0;
    for (WT_DATA_HANDLE *dhandle : walked) {
        t.make_walk_point(dhandle);
        btree_of(dhandle)->evict_walk_ends = 1;
        int64_t pos = t.position(dhandle);
        if (pos > last)
            last = pos;
    }
    REQUIRE(t.hazards() == walked.size());

    /*
     * Visiting a tree resets its end count, so an end count on a tree past the last walk point
     * shows whether the release went on after it.
     */
    WT_DATA_HANDLE *beyond = t.dhandles[0];
    REQUIRE(t.position(beyond) > last);
    btree_of(beyond)->evict_walk_ends = 1;

    t.clear_all();

    for (WT_DATA_HANDLE *dhandle : walked) {
        CHECK(btree_of(dhandle)->evict_ref == nullptr);
        CHECK(btree_of(dhandle)->evict_walk_ends == 0);
    }
    CHECK(t.hazards() == 0);
    CHECK(btree_of(beyond)->evict_walk_ends == 1);
}

TEST_CASE("Skipping idle trees lets a waiting writer take the handle list lock", "[evict]")
{
    /* Enough idle trees that skipping them usually outlasts the writer queueing. */
    walk_test t(50);
    WT_CONNECTION *wt_conn = t.conn_wrapper.get_wt_connection();
    WT_SESSION *wt_writer;
    REQUIRE(wt_conn->open_session(wt_conn, nullptr, nullptr, &wt_writer) == 0);
    WT_SESSION_IMPL *writer = (WT_SESSION_IMPL *)wt_writer;
    WT_RWLOCK *dhandle_lock = &t.conn->dhandle_lock;

    /* Target no dimension, so the scan finds nothing of interest and skips every tree early. */
    uint32_t saved_flags = t.evict->flags;
    FLD_CLR(t.evict->flags, WT_EVICT_CACHE_ALL);

    /*
     * Run a scan and, once it holds the handle list lock for reading, queue for the lock as a
     * writer. Return whether the writer got the lock while the scan was still running and had saved
     * one of the test's tables: the scan saves the tree it skips when it lets a writer in. The only
     * other place the scan saves a tree and releases the lock is before walking a tree it wants,
     * and the test's tables have a root page and nothing of interest, so the scan never wants them.
     * A scan that never lets the writer in fails every attempt.
     */
    auto writer_got_in_during_scan = [&]() {
        std::atomic<bool> scanning{false}, scan_done{false};
        bool done_when_locked = true;
        WT_DATA_HANDLE *saved_when_locked = nullptr;

        t.clear_all();
        std::thread writer_thread([&] {
            while (!scanning)
                std::this_thread::yield();
            for (;;) {
                WT_RWLOCK lock;
                lock.u.v = __wt_atomic_load_uint64_v_relaxed(&dhandle_lock->u.v);
                if (lock.u.s.readers_active != 0 || scan_done)
                    break;
            }
            __wt_writelock(writer, dhandle_lock);
            done_when_locked = scan_done;
            saved_when_locked = __wt_atomic_load_ptr_relaxed(&t.evict->walk_tree);
            __wt_writeunlock(writer, dhandle_lock);
        });

        scanning = true;
        int ret = __wti_evict_walk(t.evict->walk_session, t.evict->evict_fill_queue);
        scan_done = true;
        writer_thread.join();

        /* No page was queued for eviction. */
        CHECK(ret == WT_NOTFOUND);
        return (!done_when_locked &&
          std::find(t.dhandles.begin(), t.dhandles.end(), saved_when_locked) != t.dhandles.end());
    };

    /* The writer queueing in time is a race, so give it several scans. */
    bool got_in = false;
    for (int attempt = 0; attempt < 20 && !got_in; ++attempt)
        got_in = writer_got_in_during_scan();
    t.evict->flags = saved_flags;
    CHECK(got_in);
}

TEST_CASE("A walk that moves on to the root does not save it as a walk point", "[evict]")
{
    const char *uri = "file:test_evict_root_walk_point.wt";
    const int nrows = 200;
    WT_CURSOR *reader = nullptr;

    /* A tree of small leaf pages, all on disk after the bulk load. */
    walk_test t(0, [&](WT_SESSION *wt_session) {
        WT_CURSOR *bulk;
        std::string value(100, 'v');
        REQUIRE(wt_session->create(wt_session, uri,
                  "key_format=i,value_format=S,allocation_size=512,leaf_page_max=512") == 0);
        REQUIRE(wt_session->open_cursor(wt_session, uri, nullptr, "bulk", &bulk) == 0);
        for (int k = 1; k <= nrows; ++k) {
            bulk->set_key(bulk, k);
            bulk->set_value(bulk, value.c_str());
            REQUIRE(bulk->insert(bulk) == 0);
        }
        REQUIRE(bulk->close(bulk) == 0);
        REQUIRE(wt_session->open_cursor(wt_session, uri, nullptr, nullptr, &reader) == 0);
    });
    t.cursors.push_back(reader);
    WT_DATA_HANDLE *dhandle = ((WT_CURSOR_BTREE *)reader)->dhandle;
    WT_BTREE *btree = btree_of(dhandle);

    /* Read the last two leaf pages into the cache: the last child of the root and the one before.
     */
    auto leaf_of = [&](int key) {
        reader->set_key(reader, key);
        REQUIRE(reader->search(reader) == 0);
        WT_REF *ref = ((WT_CURSOR_BTREE *)reader)->ref;
        REQUIRE(reader->reset(reader) == 0);
        return (ref);
    };
    WT_REF *last = leaf_of(nrows);
    WT_REF *before_last = nullptr;
    for (int key = nrows - 1; key > 0 && before_last == nullptr; --key)
        if (leaf_of(key) != last)
            before_last = leaf_of(key);
    REQUIRE(before_last != nullptr);
    WT_PAGE_INDEX *pindex;
    WT_INTL_INDEX_GET_SAFE(btree->root.page, pindex);
    REQUIRE(pindex->entries >= 3);
    REQUIRE(pindex->index[pindex->entries - 1] == last);
    REQUIRE(pindex->index[pindex->entries - 2] == before_last);

    /*
     * Leave the walk point on the leaf before the last one, and mark the last one as not needed:
     * the walk queues the first page it looks at, finds the queue full on the last leaf, and moves
     * past it, which leaves it on the root.
     */
    WT_SESSION_IMPL *walk_session = t.evict->walk_session;
    WT_REF *ref = nullptr;
    int ret = 0;
    do
        WT_WITH_DHANDLE(walk_session, dhandle,
          ret = __wt_tree_walk(walk_session, &ref,
            WT_READ_EVICT_WALK_FLAGS | WT_READ_SKIP_INTL | WT_READ_VISIBLE_ALL));
    while (ret == 0 && ref != nullptr && ref != before_last);
    REQUIRE(ret == 0);
    REQUIRE(ref == before_last);
    __wt_atomic_store_ptr_relaxed(&btree->evict_ref, ref);
    __wt_atomic_store_uint64_relaxed(&last->page->read_gen, WT_READGEN_WONT_NEED);
    btree->evict_start_type = WT_EVICT_WALK_NEXT;
    btree->evict_walk_ends = 0;
    btree->evict_walk_progress = btree->evict_walk_target = 0;
    __wt_atomic_store_uint32_relaxed(&btree->evict_walk_period, 0);

    /* Look for clean pages only, with room for one more candidate, and scan this tree first. */
    uint32_t saved_flags = t.evict->flags;
    t.evict->flags = WT_EVICT_CACHE_CLEAN;
    WTI_EVICT_QUEUE *queue = t.evict->evict_fill_queue;
    uint32_t saved_entries = queue->evict_entries;
    uint32_t cap = WT_MIN(
      t.evict->evict_slots, 1 + (uint32_t)(__wt_cache_pages_inuse(S2C(t.session)->cache) / 2));
    REQUIRE(cap >= 1);
    uint32_t slot = cap - 1;
    REQUIRE(slot >= saved_entries);
    REQUIRE(queue->evict_queue[slot].ref == nullptr);
    queue->evict_entries = slot;
    __wti_evict_set_saved_walk_tree(t.session, dhandle);

    ret = __wti_evict_walk(walk_session, queue);
    uint32_t queued = queue->evict_entries - slot;
    WT_REF *queued_ref = queue->evict_queue[slot].ref;
    WT_REF *saved = __wt_atomic_load_ptr_relaxed(&btree->evict_ref);

    /* Put the queue and the eviction flags back before checking anything. */
    if (queued != 0)
        __evict_list_clear(t.session, &queue->evict_queue[slot]);
    queue->evict_entries = saved_entries;
    t.evict->flags = saved_flags;

    CHECK(ret == 0);
    /* The walk queued the leaf it started on, so it ended on the last leaf and moved past it. */
    REQUIRE(queued == 1);
    REQUIRE(queued_ref == before_last);
    CHECK(saved != &btree->root);
    CHECK(saved == nullptr);
    /* Every walk point holds a hazard pointer, so none is left once the walk point is gone. */
    CHECK(t.hazards() == 0);
}
