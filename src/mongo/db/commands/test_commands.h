// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/durable_history_pin.h"
#include "mongo/util/modules.h"

#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * This hook pairs with the `pinHistoryReplicated` test-only command. The test command will pin the
 * oldest timestamp and perform a write into `mdb_testing.pinned_timestamp`. This hook knows how to
 * read that collection and re-pin any history requests after a restart or across rollback.
 */
class [[MONGO_MOD_PUBLIC]] TestingDurableHistoryPin : public DurableHistoryPin {
public:
    std::string getName() override;
    boost::optional<Timestamp> calculatePin(OperationContext* opCtx) override;
};
}  // namespace mongo
