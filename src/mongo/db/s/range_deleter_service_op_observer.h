// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {

/**
 * OpObserver for Range Deleter Service.
 * Observes all writes to the config.rangeDeletions namespace and schedule/remove range deletion
 * tasks accordingly.
 */
class RangeDeleterServiceOpObserver final : public OpObserverNoop {
    RangeDeleterServiceOpObserver(const RangeDeleterServiceOpObserver&) = delete;
    RangeDeleterServiceOpObserver& operator=(const RangeDeleterServiceOpObserver&) = delete;

public:
    RangeDeleterServiceOpObserver();
    ~RangeDeleterServiceOpObserver() override;

    NamespaceFilters getNamespaceFilters() const final {
        return {NamespaceFilter::kConfig, NamespaceFilter::kConfig};
    }

    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   const std::vector<RecordId>& recordIds,
                   std::vector<bool> fromMigrate,
                   bool defaultFromMigrate,
                   OpStateAccumulator* opAccumulator = nullptr) override;

    void onUpdate(OperationContext* opCtx,
                  const OplogUpdateEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) override;
};

}  // namespace mongo
