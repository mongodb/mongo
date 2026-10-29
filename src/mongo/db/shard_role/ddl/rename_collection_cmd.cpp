// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/rename_collection_common.h"
#include "mongo/db/shard_role/ddl/rename_collection_gen.h"
#include "mongo/db/shard_role/ddl/replica_set_ddl_tracker.h"
#include "mongo/db/shard_role/shard_catalog/rename_collection.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <iosfwd>
#include <memory>
#include <set>
#include <string>
#include <variant>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

using std::string;
using std::stringstream;

namespace {

class CmdRenameCollection final : public TypedCommand<CmdRenameCollection> {
public:
    using Request = RenameCollectionCommand;

    const std::set<std::string>& apiVersions() const override {
        return kApiVersions1;
    }

    bool adminOnly() const override {
        return true;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    std::string help() const override {
        return " example: { renameCollection: foo.a, to: bar.b }";
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        void typedRun(OperationContext* opCtx) {
            const auto& fromNss = getFrom();
            const auto& toNss = request().getTo();

            ReplicaSetDDLTracker::ScopedReplicaSetDDL scopedReplicaSetDDL(
                opCtx, {fromNss, toNss}, definition()->getName(), {.acquireDDLLocks = true});

            uassert(ErrorCodes::IllegalOperation,
                    "Can't rename a collection to itself",
                    fromNss != toNss);

            RenameCollectionOptions options;
            options.stayTemp = request().getStayTemp();
            options.expectedSourceUUID = request().getCollectionUUID();
            visit(OverloadedVisitor{
                      [&options](bool dropTarget) { options.dropTarget = dropTarget; },
                      [&options](const UUID& uuid) {
                          options.dropTarget = true;
                          options.expectedTargetUUID = uuid;
                      },
                  },
                  request().getDropTarget());

            validateAndRunRenameCollection(opCtx, fromNss, toNss, options);
        }

    private:
        const NamespaceString& getFrom() const {
            return request().getCommandParameter();
        }

        NamespaceString ns() const override {
            return getFrom();
        }

        const DatabaseName& db() const override {
            return request().getDbName();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassertStatusOK(rename_collection::checkAuthForRenameCollectionCommand(
                opCtx->getClient(), request()));
        }
    };
};
MONGO_REGISTER_COMMAND(CmdRenameCollection).forShard();

}  // namespace
}  // namespace mongo
