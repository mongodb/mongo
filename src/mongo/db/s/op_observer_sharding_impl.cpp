/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/s/op_observer_sharding_impl.h"

//#include "mongo/bson/bsonobjbuilder.h"
//#include "mongo/db/auth/authorization_manager.h"
//#include "mongo/db/catalog/collection_catalog_entry.h"
//#include "mongo/db/catalog/collection_options.h"
//#include "mongo/db/catalog/database.h"
//#include "mongo/db/catalog/database_holder.h"
//#include "mongo/db/catalog/namespace_uuid_cache.h"
//#include "mongo/db/commands/feature_compatibility_version.h"
//#include "mongo/db/commands/feature_compatibility_version_parser.h"
//#include "mongo/db/concurrency/d_concurrency.h"
//#include "mongo/db/concurrency/write_conflict_exception.h"
//#include "mongo/db/index/index_descriptor.h"
//#include "mongo/db/logical_time_validator.h"
//#include "mongo/db/namespace_string.h"
//#include "mongo/db/operation_context.h"
//#include "mongo/db/repl/oplog.h"
//#include "mongo/db/repl/oplog_entry_gen.h"
//#include "mongo/db/repl/replication_coordinator.h"
////#include "mongo/db/s/shard_server_op_observer.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_source_manager.h"
//#include "mongo/db/server_options.h"
//#include "mongo/db/session_catalog.h"
//#include "mongo/db/transaction_participant.h"
//#include "mongo/db/views/durable_view_catalog.h"
//#include "mongo/scripting/engine.h"
//#include "mongo/util/assert_util.h"
//#include "mongo/util/fail_point_service.h"

namespace mongo {
namespace {
const auto getIsMigrating = OperationContext::declareDecoration<bool>();
}

bool OpObserverShardingImpl::isMigrating(OperationContext* opCtx,
                                         NamespaceString const& nss,
                                         BSONObj const& docToDelete) {
    auto css = CollectionShardingRuntime::get(opCtx, nss);
    auto msm = MigrationSourceManager::get(css);
    return msm && msm->getCloner()->isDocumentInMigratingChunk(docToDelete);
}

void OpObserverShardingImpl::shardObserveAboutToDelete(OperationContext* opCtx,
                                                       NamespaceString const& nss,
                                                       BSONObj const& docToDelete) {
    getIsMigrating(opCtx) = isMigrating(opCtx, nss, docToDelete);
}

void OpObserverShardingImpl::shardObserveInsertOp(OperationContext* opCtx,
                                                  const NamespaceString nss,
                                                  const BSONObj& insertedDoc,
                                                  const repl::OpTime& opTime,
                                                  const bool fromMigrate) {
    auto* const css = (nss == NamespaceString::kSessionTransactionsTableNamespace || fromMigrate)
        ? nullptr
        : CollectionShardingRuntime::get(opCtx, nss);
    if (css) {
        css->checkShardVersionOrThrow(opCtx);
        auto msm = MigrationSourceManager::get(css);
        if (msm) {
            msm->getCloner()->onInsertOp(opCtx, insertedDoc, opTime);
        }
    }
}

void OpObserverShardingImpl::shardObserveUpdateOp(OperationContext* opCtx,
                                                  const NamespaceString nss,
                                                  const BSONObj& updatedDoc,
                                                  const repl::OpTime& opTime,
                                                  const repl::OpTime& prePostImageOpTime) {
    auto* const css = CollectionShardingRuntime::get(opCtx, nss);
    css->checkShardVersionOrThrow(opCtx);
    auto msm = MigrationSourceManager::get(css);
    if (msm) {
        msm->getCloner()->onUpdateOp(opCtx, updatedDoc, opTime, prePostImageOpTime);
    }
}

void OpObserverShardingImpl::shardObserveDeleteOp(OperationContext* opCtx,
                                                  const NamespaceString nss,
                                                  const BSONObj& documentKey,
                                                  const repl::OpTime& opTime,
                                                  const repl::OpTime& preImageOpTime) {
    auto& isMigrating = getIsMigrating(opCtx);
    auto* const css = CollectionShardingRuntime::get(opCtx, nss);
    css->checkShardVersionOrThrow(opCtx);
    auto msm = MigrationSourceManager::get(css);
    if (msm && isMigrating) {
        msm->getCloner()->onDeleteOp(opCtx, documentKey, opTime, preImageOpTime);
    }
}

}  // namespace mongo
