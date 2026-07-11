// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/s/resharding/resharding_metrics_common.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/storage/durable_history_pin.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

class [[MONGO_MOD_PUBLIC]] ReshardingHistoryHook : public DurableHistoryPin {
public:
    static constexpr std::string_view kName = "resharding"sv;

    std::string getName() override {
        return std::string{kName};
    }

    boost::optional<Timestamp> calculatePin(OperationContext* opCtx) override;
};

/**
 * OpObserver for observing writes to internal resharding collections. This includes collections
 * such as config.reshardingOperations, config.localReshardingOperations.donor, and
 * config.localReshardingOperations.recipient.
 */
class [[MONGO_MOD_PUBLIC]] ReshardingOpObserver final : public OpObserverNoop {
    ReshardingOpObserver(const ReshardingOpObserver&) = delete;
    ReshardingOpObserver& operator=(const ReshardingOpObserver&) = delete;

public:
    using Role = ReshardingMetricsCommon::Role;

    ReshardingOpObserver();
    ~ReshardingOpObserver() override;

    NamespaceFilters getNamespaceFilters() const final {
        return {NamespaceFilter::kConfigAndSystem, NamespaceFilter::kConfigAndSystem};
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

    void onDelete(OperationContext* opCtx,
                  const CollectionPtr& coll,
                  StmtId stmtId,
                  const BSONObj& doc,
                  const DocumentKey& documentKey,
                  const OplogDeleteEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) override;

    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  bool markFromMigrate,
                                  bool isTimeseries) override;

private:
    const stdx::unordered_map<NamespaceString, Role> _nssToRoleMap{
        {NamespaceString::kConfigReshardingOperationsNamespace, Role::kCoordinator},
        {NamespaceString::kDonorReshardingOperationsNamespace, Role::kDonor},
        {NamespaceString::kRecipientReshardingOperationsNamespace, Role::kRecipient},
    };
};

}  // namespace mongo
