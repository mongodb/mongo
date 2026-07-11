// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstdint>

namespace mongo {
namespace repl {

class PrimaryOnlyServiceRegistry;

/**
 * OpObserver for PrimaryOnlyService.
 */
class [[MONGO_MOD_PUBLIC]] PrimaryOnlyServiceOpObserver final : public OpObserverNoop {
    PrimaryOnlyServiceOpObserver(const PrimaryOnlyServiceOpObserver&) = delete;
    PrimaryOnlyServiceOpObserver& operator=(const PrimaryOnlyServiceOpObserver&) = delete;

public:
    explicit PrimaryOnlyServiceOpObserver(ServiceContext* serviceContext);
    ~PrimaryOnlyServiceOpObserver() override;

    NamespaceFilters getNamespaceFilters() const final {
        return {/*update=*/NamespaceFilter::kNone, /*delete=*/NamespaceFilter::kConfig};
    }

    void onDelete(OperationContext* opCtx,
                  const CollectionPtr& coll,
                  StmtId stmtId,
                  const BSONObj& doc,
                  const DocumentKey& documentKey,
                  const OplogDeleteEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) final;

    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  bool markFromMigrate,
                                  bool isViewlessTimeseries) final;

private:
    PrimaryOnlyServiceRegistry* _registry;
};

}  // namespace repl
}  // namespace mongo
