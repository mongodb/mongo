// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/feature_compatibility_version_document_gen.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/modules.h"
#include "mongo/util/version/releases.h"

#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * OpObserver for Feature Compatibility Version (FCV).
 * Observes all writes to the FCV document under admin.system.version and sets the in-memory FCV
 * value.
 */
class FcvOpObserver final : public OpObserverNoop {
    FcvOpObserver(const FcvOpObserver&) = delete;
    FcvOpObserver& operator=(const FcvOpObserver&) = delete;

public:
    FcvOpObserver() = default;
    ~FcvOpObserver() override = default;

    // FcvOpObserver overrides.

    NamespaceFilters getNamespaceFilters() const final {
        return {NamespaceFilter::kSystem, NamespaceFilter::kSystem};
    }

    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator first,
                   std::vector<InsertStatement>::const_iterator last,
                   const std::vector<RecordId>& recordIds,
                   std::vector<bool> fromMigrate,
                   bool defaultFromMigrate,
                   OpStateAccumulator* opAccumulator = nullptr) final;

    void onUpdate(OperationContext* opCtx,
                  const OplogUpdateEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) final;

    void onDelete(OperationContext* opCtx,
                  const CollectionPtr& coll,
                  StmtId stmtId,
                  const BSONObj& doc,
                  const DocumentKey& documentKey,
                  const OplogDeleteEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) final;

    void onReplicationRollback(OperationContext* opCtx, const RollbackObserverInfo& rbInfo) final;

private:
    /**
     * Set the FCV to newVersion, making sure to close any outgoing connections with incompatible
     * servers and closing open transactions if necessary. Increments the server TopologyVersion.
     */
    static void _setVersion(OperationContext* opCtx,
                            const FeatureCompatibilityVersionDocument& fcvDoc,
                            bool onRollback,
                            bool withinRecoveryUnit,
                            boost::optional<Timestamp> commitTs = boost::none);

    /**
     * Examines a document inserted or updated in the server configuration collection
     * (admin.system.version). If it is the featureCompatibilityVersion document, validates the
     * document and on commit, updates the server parameter.
     */
    static void _onInsertOrUpdate(OperationContext* opCtx, const BSONObj& doc);
};

}  // namespace mongo
