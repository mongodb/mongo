/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

// Standard include:
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

// External include:
#include <catch2/catch.hpp>

// WiredTiger include:
#include "wt_internal.h"
#include "wrappers/mock_session.h"

namespace truncate_list_helpers {

using truncate_range = std::pair<WT_ITEM, WT_ITEM>;

class scoped_fast_truncate_enable {
public:
    scoped_fast_truncate_enable() : _previous(__wt_process.disagg_fast_truncate_2026)
    {
        __wt_process.disagg_fast_truncate_2026 = true;
    }

    ~scoped_fast_truncate_enable()
    {
        __wt_process.disagg_fast_truncate_2026 = _previous;
    }

private:
    bool _previous;
};

[[nodiscard]] WT_ITEM make_item(std::string_view view);

[[nodiscard]] std::string_view as_view(const WT_ITEM &item);

[[nodiscard]] WT_TRUNCATE *truncate_list_head(WT_LAYERED_TABLE &table);

[[nodiscard]] size_t truncate_list_size(const WT_LAYERED_TABLE &table);

[[nodiscard]] bool lock_is_released(WT_SESSION_IMPL &session, WT_LAYERED_TABLE &table);

[[nodiscard]] WT_TXN_OP *last_txn_op(WT_SESSION_IMPL &session);

class truncate_list_fixture {
public:
    truncate_list_fixture();
    ~truncate_list_fixture();

    [[nodiscard]] WT_SESSION_IMPL &
    session() const
    {
        return *_session;
    }

    [[nodiscard]] WT_LAYERED_TABLE &
    layered_table()
    {
        return _table;
    }

    [[nodiscard]] uint32_t
    reference_count() const
    {
        return __wt_atomic_load_uint32_relaxed(&_table.iface.references);
    }

    WT_TRUNCATE *add_entry(const WT_ITEM &start, const WT_ITEM &stop);

private:
    scoped_fast_truncate_enable _enable;
    std::shared_ptr<mock_session> _mock;
    WT_SESSION_IMPL *_session{};
    mutable WT_LAYERED_TABLE _table{};
};

} // namespace truncate_list_helpers
