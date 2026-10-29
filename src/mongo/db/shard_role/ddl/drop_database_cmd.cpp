// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/coll_mod_gen.h"
#include "mongo/db/shard_role/ddl/drop_database_gen.h"
#include "mongo/db/shard_role/ddl/drop_gen.h"
#include "mongo/db/shard_role/ddl/replica_set_ddl_tracker.h"
#include "mongo/db/shard_role/shard_catalog/drop_collection.h"
#include "mongo/db/shard_role/shard_catalog/drop_database.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

class CmdDropDatabase : public DropDatabaseCmdVersion1Gen<CmdDropDatabase> {
public:
    std::string help() const final {
        return "drop (delete) this database";
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kNever;
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
            return NamespaceString(request().getDbName());
        }
        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to drop database '"
                                  << request().getDbName().toStringForErrorMsg() << "'",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnNamespace(ns(), ActionType::dropDatabase));
        }
        Reply typedRun(OperationContext* opCtx) final {
            ReplicaSetDDLTracker::ScopedReplicaSetDDL scopedReplicaSetDDL(
                opCtx, {ns()}, definition()->getName(), {.acquireDDLLocks = true});

            auto dbName = request().getDbName();
            // disallow dropping the config database
            if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) &&
                (dbName == DatabaseName::kConfig)) {
                uasserted(ErrorCodes::IllegalOperation,
                          "Cannot drop 'config' database if mongod started "
                          "with --configsvr");
            }

            if ((repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet()) &&
                (dbName == DatabaseName::kLocal)) {
                uasserted(ErrorCodes::IllegalOperation,
                          str::stream() << "Cannot drop '" << dbName.toStringForErrorMsg()
                                        << "' database while replication is active");
            }

            if (request().getCommandParameter() != 1) {
                uasserted(5255100, "Have to pass 1 as 'drop' parameter");
            }

            Status status = dropDatabase(opCtx, dbName);
            if (status != ErrorCodes::NamespaceNotFound) {
                uassertStatusOK(status);
            }
            return {};
        }
    };
};
MONGO_REGISTER_COMMAND(CmdDropDatabase).forShard();

}  // namespace
}  // namespace mongo
