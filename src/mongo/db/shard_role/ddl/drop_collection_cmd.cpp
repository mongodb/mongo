/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/coll_mod_gen.h"
#include "mongo/db/shard_role/ddl/drop_database_gen.h"
#include "mongo/db/shard_role/ddl/drop_gen.h"
#include "mongo/db/shard_role/ddl/replica_set_ddl_tracker.h"
#include "mongo/db/shard_role/shard_catalog/drop_collection.h"
#include "mongo/db/shard_role/shard_catalog/drop_database.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

class CmdDrop : public DropCmdVersion1Gen<CmdDrop> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
    }
    bool adminOnly() const final {
        return false;
    }
    std::string help() const final {
        return "drop a collection\n{drop : <collectionName>}";
    }
    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }
    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;
        bool supportsWriteConcern() const final {
            return true;
        }
        NamespaceString ns() const final {
            return request().getNamespace();
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto ns = request().getNamespace();
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to drop collection '"
                                  << ns.toStringForErrorMsg() << "'",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnNamespace(ns, ActionType::dropCollection));
        }
        Reply typedRun(OperationContext* opCtx) final {
            ReplicaSetDDLTracker::ScopedReplicaSetDDL scopedReplicaSetDDL(
                opCtx, std::vector<NamespaceString>{ns()});

            if (request().getNamespace().isOplog()) {
                uassert(5255000,
                        "can't drop live oplog while replicating",
                        !repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet());
                auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
                invariant(storageEngine);
                // We use the method supportsRecoveryTimestamp() to detect whether we are using
                // the WiredTiger storage engine, which is currently only storage engine that
                // supports the replSetResizeOplog command.
                uassert(
                    5255001,
                    "can't drop oplog on storage engines that support replSetResizeOplog command",
                    !storageEngine->supportsRecoveryTimestamp());
            }

            // We need to copy the serialization context from the request to the reply object
            Reply reply(
                SerializationContext::stateCommandReply(request().getSerializationContext()));
            uassertStatusOK(
                dropCollection(opCtx,
                               request().getNamespace(),
                               request().getCollectionUUID(),
                               &reply,
                               DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));
            return reply;
        }
    };
};
MONGO_REGISTER_COMMAND(CmdDrop).forShard();

}  // namespace
}  // namespace mongo
