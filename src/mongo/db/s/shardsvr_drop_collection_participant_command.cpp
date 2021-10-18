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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/drop_collection_coordinator.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {
namespace {

class ShardsvrDropCollectionParticipantCommand final
    : public TypedCommand<ShardsvrDropCollectionParticipantCommand> {
public:
    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return Command::AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Internal command, which is exported by secondary sharding servers. Do not call "
               "directly. Participates in droping a collection.";
    }

    using Request = ShardsvrDropCollectionParticipant;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << Request::kCommandName
                                  << " must be called with majority writeConcern, got "
                                  << opCtx->getWriteConcern().wMode,
                    opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

            opCtx->setAlwaysInterruptAtStepDownOrUp();

            try {
                DropCollectionCoordinator::dropCollectionLocally(opCtx, ns());
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                LOGV2_DEBUG(5280920,
                            1,
                            "Namespace not found while trying to delete local collection",
                            "namespace"_attr = ns());
            }

            // The txnParticipant will only be missing when the command was sent from a coordinator
            // running an old 5.0.0 binary that didn't attach a sessionId & txnNumber.
            // TODO SERVER-60773: Once 6.0 has branched out, txnParticipant must always exist. Add a
            // uassert for that.
            auto txnParticipant = TransactionParticipant::get(opCtx);
            if (txnParticipant) {
                // Since no write that generated a retryable write oplog entry with this sessionId
                // and txnNumber happened, we need to make a dummy write so that the session gets
                // durably persisted on the oplog. This must be the last operation done on this
                // command.
                DBDirectClient client(opCtx);
                client.update(NamespaceString::kServerConfigurationNamespace.ns(),
                              BSON("_id" << Request::kCommandName),
                              BSON("$inc" << BSON("count" << 1)),
                              true /* upsert */,
                              false /* multi */);
            }
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };
} sharsvrdDropCollectionParticipantCommand;

}  // namespace
}  // namespace mongo
