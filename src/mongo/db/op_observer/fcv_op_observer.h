/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/util/version/releases.h"

#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

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
                            multiversion::FeatureCompatibilityVersion newVersion,
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
