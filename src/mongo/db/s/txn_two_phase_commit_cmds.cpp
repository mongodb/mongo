/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/operation_context_session_mongod.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/transaction_coordinator_service.h"
#include "mongo/db/transaction_participant.h"

namespace mongo {
namespace {

class CmdPrepareTxn : public BasicCommand {
public:
    CmdPrepareTxn() : BasicCommand("prepareTransaction") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool adminOnly() const {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Prepares a transaction. This is only expected to be called by mongos.";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto txnParticipant = TransactionParticipant::get(opCtx);
        uassert(ErrorCodes::CommandFailed,
                "prepareTransaction must be run within a transaction",
                txnParticipant);

        uassert(ErrorCodes::CommandNotSupported,
                "'prepareTransaction' is only supported in feature compatibility version 4.2",
                (serverGlobalParams.featureCompatibility.getVersion() ==
                 ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42));

        uassert(ErrorCodes::NoSuchTransaction,
                "Transaction isn't in progress",
                txnParticipant->inMultiDocumentTransaction());

        if (txnParticipant->transactionIsPrepared()) {
            auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
            auto prepareOpTime = txnParticipant->getPrepareOpTime();
            // Set the client optime to be prepareOpTime if it's not already later than
            // prepareOpTime.
            // This ensures that we wait for writeConcern and that prepareOpTime will be committed.
            if (prepareOpTime > replClient.getLastOp()) {
                replClient.setLastOp(prepareOpTime);
            }

            invariant(opCtx->recoveryUnit()->getPrepareTimestamp() == prepareOpTime.getTimestamp(),
                      str::stream() << "recovery unit prepareTimestamp: "
                                    << opCtx->recoveryUnit()->getPrepareTimestamp().toString()
                                    << " participant prepareOpTime: "
                                    << prepareOpTime.toString());

            result.append("prepareTimestamp", prepareOpTime.getTimestamp());
            return true;
        }

        // Add prepareTimestamp to the command response.
        auto timestamp = txnParticipant->prepareTransaction(opCtx);
        result.append("prepareTimestamp", timestamp);

        return true;
    }
} prepareTransactionCmd;

class VoteCommitTransactionCmd : public TypedCommand<VoteCommitTransactionCmd> {
public:
    using Request = VoteCommitTransaction;
    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            // Only config servers or initialized shard servers can act as transaction coordinators.
            if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
                uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());
            }

            uassert(
                ErrorCodes::CommandNotSupported,
                "'voteCommitTransaction' is only supported in feature compatibility version 4.2",
                (serverGlobalParams.featureCompatibility.getVersion() ==
                 ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42));

            const auto& cmd = request();

            TransactionCoordinatorService::get(opCtx)->voteCommit(
                opCtx,
                opCtx->getLogicalSessionId().get(),
                opCtx->getTxnNumber().get(),
                cmd.getShardId(),
                cmd.getPrepareTimestamp());
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {}
    };

    virtual bool adminOnly() const {
        return true;
    }

    std::string help() const override {
        return "Votes to commit a transaction; sent by a transaction participant to the "
               "transaction commit coordinator for a cross-shard transaction";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
} voteCommitTransactionCmd;

class VoteAbortTransactionCmd : public TypedCommand<VoteAbortTransactionCmd> {
public:
    using Request = VoteAbortTransaction;
    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            // Only config servers or initialized shard servers can act as transaction coordinators.
            if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
                uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());
            }

            uassert(ErrorCodes::CommandNotSupported,
                    "'voteAbortTransaction' is only supported in feature compatibility version 4.2",
                    (serverGlobalParams.featureCompatibility.getVersion() ==
                     ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42));

            const auto& cmd = request();

            TransactionCoordinatorService::get(opCtx)->voteAbort(opCtx,
                                                                 opCtx->getLogicalSessionId().get(),
                                                                 opCtx->getTxnNumber().get(),
                                                                 cmd.getShardId());
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {}
    };

    virtual bool adminOnly() const {
        return true;
    }

    std::string help() const override {
        return "Votes to abort a transaction; sent by a transaction participant to the transaction "
               "commit coordinator for a cross-shard transaction";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
} voteAbortTransactionCmd;

class CoordinateCommitTransactionCmd : public TypedCommand<CoordinateCommitTransactionCmd> {
public:
    using Request = CoordinateCommitTransaction;
    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            // Only config servers or initialized shard servers can act as transaction coordinators.
            if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
                uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());
            }

            uassert(ErrorCodes::CommandNotSupported,
                    "'coordinateCommitTransaction' is only supported in feature compatibility "
                    "version 4.2",
                    (serverGlobalParams.featureCompatibility.getVersion() ==
                     ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42));

            const auto& cmd = request();

            // Convert the participant list array into a set, and assert that all participants in
            // the list are unique.
            // TODO (PM-564): Propagate the 'readOnly' flag down into the TransactionCoordinator.
            std::set<ShardId> participantList;
            for (const auto& participant : cmd.getParticipants()) {
                const auto shardId = participant.getShardId();
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "participant list contained duplicate shardId " << shardId,
                        std::find(participantList.begin(), participantList.end(), shardId) ==
                            participantList.end());
                participantList.insert(shardId);
            }

            TransactionCoordinatorService::get(opCtx)->coordinateCommit(
                opCtx,
                opCtx->getLogicalSessionId().get(),
                opCtx->getTxnNumber().get(),
                participantList);

            // Execute the 'prepare' logic on the local participant (the router does not send a
            // separate 'prepare' message to the coordinator shard.
            {
                OperationContextSessionMongod checkOutSession(
                    opCtx, true, false, boost::none, false);

                auto txnParticipant = TransactionParticipant::get(opCtx);

                txnParticipant->unstashTransactionResources(opCtx, "prepareTransaction");
                ScopeGuard guard = MakeGuard([&txnParticipant, opCtx]() {
                    txnParticipant->abortActiveUnpreparedOrStashPreparedTransaction(opCtx);
                });

                txnParticipant->prepareTransaction(opCtx);

                txnParticipant->stashTransactionResources(opCtx);
                guard.Dismiss();
            }
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {}
    };

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Coordinates the commit for a transaction. Only called by mongos.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
} coordinateCommitTransactionCmd;

}  // namespace
}  // namespace mongo
