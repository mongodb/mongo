/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

// Standard include:
#include <memory>
#include <string_view>

// WiredTiger include:
#include "wt_internal.h"
#include "truncate_list_helpers.hpp"

namespace truncate_list_helpers {

WT_ITEM
make_item(const std::string_view view)
{
    WT_ITEM item{};
    item.data = view.data();
    item.size = view.size();
    return item;
}

std::string_view
as_view(const WT_ITEM &item)
{
    return {static_cast<const char *>(item.data), item.size};
}

WT_TRUNCATE *
truncate_list_head(WT_LAYERED_TABLE &table)
{
    return TAILQ_FIRST(&table.truncateqh);
}

size_t
truncate_list_size(const WT_LAYERED_TABLE &table)
{
    size_t count = 0;
    WT_TRUNCATE *entry = nullptr;

    TAILQ_FOREACH (entry, &table.truncateqh, q) {
        ++count;
    }

    return count;
}

bool
lock_is_released(WT_SESSION_IMPL &session, WT_LAYERED_TABLE &table)
{
    if (__wt_try_writelock(&session, &table.truncate_lock) != 0)
        return false;

    __wt_writeunlock(&session, &table.truncate_lock);
    return true;
}

WT_TXN_OP *
last_txn_op(WT_SESSION_IMPL &session)
{
    auto *txn = session.txn;
    return &txn->mod[txn->mod_count - 1];
}

truncate_list_fixture::truncate_list_fixture()
    : _mock(mock_session::build_test_mock_session()), _session(_mock->get_wt_session_impl())
{
    _table.iface.name = "layered:truncate_list_fixture";
    TAILQ_INIT(&_table.truncateqh);
    REQUIRE(__wt_rwlock_init(_session, &_table.truncate_lock) == 0);
}

truncate_list_fixture::~truncate_list_fixture()
{
    WT_TRUNCATE *entry = nullptr;

    const bool had_data = !TAILQ_EMPTY(&_table.truncateqh);

    while ((entry = TAILQ_FIRST(&_table.truncateqh)) != nullptr) {
        TAILQ_REMOVE(&_table.truncateqh, entry, q);
        __wt_free(_session, entry);
    }

    if (had_data)
        WT_DHANDLE_RELEASE(&_table.iface);

    __wt_rwlock_destroy(_session, &_table.truncate_lock);
}

WT_TRUNCATE *
truncate_list_fixture::add_entry(const WT_ITEM &start, const WT_ITEM &stop)
{
    WT_TRUNCATE *entry = nullptr;
    REQUIRE(__wt_calloc_one(_session, &entry) == 0);

    entry->layered_table = &_table;

    if (TAILQ_EMPTY(&_table.truncateqh))
        WT_DHANDLE_ACQUIRE(&_table.iface);

    // This is a shallow copy. For the purposes of the tests, we are assuming that the WT_ITEMs are
    // constructed using string literals, which have static storage duration.
    entry->start_key = start;
    entry->stop_key = stop;

    TAILQ_INSERT_TAIL(&_table.truncateqh, entry, q);
    return entry;
}

} // namespace truncate_list_helpers
