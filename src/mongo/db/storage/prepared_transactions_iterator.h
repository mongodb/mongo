// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstdint>

#include <boost/optional/optional.hpp>

namespace mongo {
/**
 * An iterator that yields the prepared_id of unclaimed prepared transactions that exist in the
 * checkpoint on startup recovery.
 */
class [[MONGO_MOD_OPEN]] PreparedTransactionsIterator {
public:
    virtual ~PreparedTransactionsIterator() = default;
    // Returns the id of a prepared transaction that has been unclaimed on startup recovery or an
    // empty boost::optional if there are no more prepared transaction ids to return.
    virtual boost::optional<uint64_t> next() = 0;
};
}  // namespace mongo
