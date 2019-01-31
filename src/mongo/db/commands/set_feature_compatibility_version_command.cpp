/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/coll_mod.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/commands/feature_compatibility_version_command_parser.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(featureCompatibilityDowngrade);
MONGO_FAIL_POINT_DEFINE(featureCompatibilityUpgrade);
MONGO_FAIL_POINT_DEFINE(pauseBeforeUpgradingSessions);
MONGO_FAIL_POINT_DEFINE(pauseBeforeDowngradingSessions);

/**
 * Returns a set of the logical session ids of each entry in config.transactions that matches the
 * given query.
 */
LogicalSessionIdSet getMatchingSessionIdsFromTransactionTable(OperationContext* opCtx,
                                                              Query query) {
    LogicalSessionIdSet sessionIds = {};

    DBDirectClient client(opCtx);
    auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace, query);
    while (cursor->more()) {
        auto txnRecord = SessionTxnRecord::parse(
            IDLParserErrorContext("setFCV-find-matching-sessions"), cursor->next());
        sessionIds.insert(txnRecord.getSessionId());
    }
    return sessionIds;
}

/**
 * Checks out each given session with a new operation context, verifies the session's transaction
 * participant passes the validation function, runs the modification function with a direct
 * client from another new operation context while the session is checked out, then invalidates the
 * session.
 */
void forEachSessionWithCheckout(
    OperationContext* opCtx,
    LogicalSessionIdSet sessionIds,
    stdx::function<bool(OperationContext* opCtx)> verifyTransactionParticipantFn,
    stdx::function<void(DBDirectClient* client, LogicalSessionId sessionId)>
        performModificationFn) {
    // Construct a new operation context to check out the session with.
    auto clientForCheckout =
        opCtx->getServiceContext()->makeClient("setFCV-transaction-table-checkout");
    AlternativeClientRegion acrForCheckout(clientForCheckout);
    for (const auto& sessionId : sessionIds) {
        // Check for interrupt on the parent opCtx because killing it won't be propagated to the
        // opCtx checking out the session and performing the modification.
        opCtx->checkForInterrupt();

        const auto opCtxForCheckout = cc().makeOperationContext();
        opCtxForCheckout->setLogicalSessionId(sessionId);
        MongoDOperationContextSession ocs(opCtxForCheckout.get());

        // Now that the session is checked out, verify it still needs to be modified using its
        // transaction participant.
        if (!verifyTransactionParticipantFn(opCtxForCheckout.get())) {
            continue;
        }

        {
            // Perform the modification on another operation context to bypass retryable writes and
            // transactions machinery.
            auto clientForModification =
                opCtx->getServiceContext()->makeClient("setFCV-transaction-table-modification");
            AlternativeClientRegion acrForModification(clientForModification);

            const auto opCtxForModification = cc().makeOperationContext();
            DBDirectClient directClient(opCtxForModification.get());
            performModificationFn(&directClient, sessionId);
        }

        // Note that invalidating the session here is unnecessary if the modification function
        // writes directly to config.transactions, which already invalidates the affected session.
        auto txnParticipant = TransactionParticipant::get(opCtxForCheckout.get());
        txnParticipant.invalidate(opCtxForCheckout.get());
    }
}

/**
 * Removes all documents from config.transactions with a "state" field because they may point to
 * oplog entries in a format a 4.0 mongod cannot process.
 */
void downgradeTransactionTable(OperationContext* opCtx) {
    // In FCV 4.0, all transaction table entries associated with a transaction have a "state" field.
    Query query(BSON("state" << BSON("$exists" << true)));
    LogicalSessionIdSet sessionIdsWithState =
        getMatchingSessionIdsFromTransactionTable(opCtx, query);

    if (MONGO_FAIL_POINT(pauseBeforeDowngradingSessions)) {
        LOG(0) << "Hit pauseBeforeDowngradingSessions failpoint";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(pauseBeforeDowngradingSessions);
    }

    // Remove all transaction table entries associated with a committed / aborted transaction. Note
    // that transactions that abort before prepare have no entry.
    forEachSessionWithCheckout(
        opCtx,
        sessionIdsWithState,
        [](OperationContext* opCtx) {
            auto txnParticipant = TransactionParticipant::get(opCtx);
            return txnParticipant.transactionIsCommitted() || txnParticipant.transactionIsAborted();
        },
        [](DBDirectClient* directClient, LogicalSessionId sessionId) {
            const auto commandResponse = directClient->runCommand([&] {
                write_ops::Delete deleteOp(NamespaceString::kSessionTransactionsTableNamespace);
                deleteOp.setDeletes({[&] {
                    write_ops::DeleteOpEntry entry;
                    entry.setQ(BSON("_id" << sessionId.toBSON()));
                    entry.setMulti(false);
                    return entry;
                }()});
                return deleteOp.serialize({});
            }());
            uassertStatusOK(getStatusFromWriteCommandReply(commandResponse->getCommandReply()));
        });
}

/**
 * Adds a "state" field to all documents in config.transactions that represent committed
 * transactions so they are in the 4.2 format.
 */
void upgradeTransactionTable(OperationContext* opCtx) {
    // Retryable writes and committed transactions have the same format in FCV 4.0, so use an empty
    // query to return all session ids in the transaction table.
    LogicalSessionIdSet allSessionIds = getMatchingSessionIdsFromTransactionTable(opCtx, Query());

    if (MONGO_FAIL_POINT(pauseBeforeUpgradingSessions)) {
        LOG(0) << "Hit pauseBeforeUpgradingSessions failpoint";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(pauseBeforeUpgradingSessions);
    }

    // Add state=committed to the transaction table entry for each session that most recently
    // committed a transaction.
    forEachSessionWithCheckout(
        opCtx,
        allSessionIds,
        [](OperationContext* opCtx) {
            auto txnParticipant = TransactionParticipant::get(opCtx);
            return txnParticipant.transactionIsCommitted();
        },
        [](DBDirectClient* directClient, LogicalSessionId sessionId) {
            const auto commandResponse = directClient->runCommand([&] {
                write_ops::Update updateOp(NamespaceString::kSessionTransactionsTableNamespace);
                updateOp.setUpdates({[&] {
                    write_ops::UpdateOpEntry entry;
                    entry.setQ(BSON("_id" << sessionId.toBSON()));
                    entry.setU(BSON("$set" << BSON("state"
                                                   << "committed")));
                    entry.setMulti(false);
                    return entry;
                }()});
                return updateOp.serialize({});
            }());
            uassertStatusOK(getStatusFromWriteCommandReply(commandResponse->getCommandReply()));
        });
}

/**
 * Sets the minimum allowed version for the cluster. If it is 4.0, then the node should not use 4.2
 * features.
 *
 * Format:
 * {
 *   setFeatureCompatibilityVersion: <string version>
 * }
 */
class SetFeatureCompatibilityVersionCommand : public BasicCommand {
public:
    SetFeatureCompatibilityVersionCommand()
        : BasicCommand(FeatureCompatibilityVersionCommandParser::kCommandName) {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        std::stringstream h;
        h << "Set the API version exposed by this node. If set to \""
          << FeatureCompatibilityVersionParser::kVersion40
          << "\", then 4.2 features are disabled. If \""
          << FeatureCompatibilityVersionParser::kVersion42
          << "\", then 4.2 features are enabled, and all nodes in the cluster must be binary "
             "version 4.2. See "
          << feature_compatibility_version_documentation::kCompatibilityLink << ".";
        return h.str();
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(),
                ActionType::setFeatureCompatibilityVersion)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        // Always wait for at least majority writeConcern to ensure all writes involved in the
        // upgrade process cannot be rolled back. There is currently no mechanism to specify a
        // default writeConcern, so we manually call waitForWriteConcern upon exiting this command.
        //
        // TODO SERVER-25778: replace this with the general mechanism for specifying a default
        // writeConcern.
        ON_BLOCK_EXIT([&] {
            // Propagate the user's wTimeout if one was given.
            auto timeout =
                opCtx->getWriteConcern().usedDefault ? INT_MAX : opCtx->getWriteConcern().wTimeout;
            WriteConcernResult res;
            auto waitForWCStatus = waitForWriteConcern(
                opCtx,
                repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp(),
                WriteConcernOptions(
                    WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, timeout),
                &res);
            CommandHelpers::appendCommandWCStatus(result, waitForWCStatus, res);
        });

        // Only allow one instance of setFeatureCompatibilityVersion to run at a time.
        invariant(!opCtx->lockState()->isLocked());
        Lock::ExclusiveLock lk(opCtx->lockState(), FeatureCompatibilityVersion::fcvLock);

        const auto requestedVersion = uassertStatusOK(
            FeatureCompatibilityVersionCommandParser::extractVersionFromCommand(getName(), cmdObj));
        ServerGlobalParams::FeatureCompatibility::Version actualVersion =
            serverGlobalParams.featureCompatibility.getVersion();

        if (requestedVersion == FeatureCompatibilityVersionParser::kVersion42) {
            uassert(ErrorCodes::IllegalOperation,
                    "cannot initiate featureCompatibilityVersion upgrade to 4.2 while a previous "
                    "featureCompatibilityVersion downgrade to 4.0 has not completed. Finish "
                    "downgrade to 4.0, then upgrade to 4.2.",
                    actualVersion !=
                        ServerGlobalParams::FeatureCompatibility::Version::kDowngradingTo40);

            if (actualVersion ==
                ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42) {
                // Set the client's last opTime to the system last opTime so no-ops wait for
                // writeConcern.
                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);
                return true;
            }

            FeatureCompatibilityVersion::setTargetUpgrade(opCtx);

            {
                // Take the global lock in S mode to create a barrier for operations taking the
                // global IX or X locks. This ensures that either
                //   - The global IX/X locked operation will start after the FCV change, see the
                //     upgrading to 4.2 FCV and act accordingly.
                //   - The global IX/X locked operation began prior to the FCV change, is acting on
                //     that assumption and will finish before upgrade procedures begin right after
                //     this.
                Lock::GlobalLock lk(opCtx, MODE_S);
            }

            updateUniqueIndexesOnUpgrade(opCtx);

            upgradeTransactionTable(opCtx);

            // Upgrade shards before config finishes its upgrade.
            if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
                uassertStatusOK(
                    ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
                        opCtx,
                        CommandHelpers::appendMajorityWriteConcern(
                            CommandHelpers::appendPassthroughFields(
                                cmdObj,
                                BSON(FeatureCompatibilityVersionCommandParser::kCommandName
                                     << requestedVersion)))));
            }

            FeatureCompatibilityVersion::unsetTargetUpgradeOrDowngrade(opCtx, requestedVersion);
        } else if (requestedVersion == FeatureCompatibilityVersionParser::kVersion40) {
            uassert(ErrorCodes::IllegalOperation,
                    "cannot initiate setting featureCompatibilityVersion to 4.0 while a previous "
                    "featureCompatibilityVersion upgrade to 4.2 has not completed.",
                    actualVersion !=
                        ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo42);

            if (actualVersion ==
                ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo40) {
                // Set the client's last opTime to the system last opTime so no-ops wait for
                // writeConcern.
                repl::ReplClientInfo::forClient(opCtx->getClient())
                    .setLastOpToSystemLastOpTime(opCtx);
                return true;
            }

            FeatureCompatibilityVersion::setTargetDowngrade(opCtx);

            {
                // Take the global lock in S mode to create a barrier for operations taking the
                // global IX or X locks. This ensures that either
                //   - The global IX/X locked operation will start after the FCV change, see the
                //     downgrading to 4.0 FCV and act accordingly.
                //   - The global IX/X locked operation began prior to the FCV change, is acting on
                //     that assumption and will finish before downgrade procedures begin right after
                //     this.
                Lock::GlobalLock lk(opCtx, MODE_S);
            }

            downgradeTransactionTable(opCtx);

            // Downgrade shards before config finishes its downgrade.
            if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
                uassertStatusOK(
                    ShardingCatalogManager::get(opCtx)->setFeatureCompatibilityVersionOnShards(
                        opCtx,
                        CommandHelpers::appendMajorityWriteConcern(
                            CommandHelpers::appendPassthroughFields(
                                cmdObj,
                                BSON(FeatureCompatibilityVersionCommandParser::kCommandName
                                     << requestedVersion)))));
            }

            FeatureCompatibilityVersion::unsetTargetUpgradeOrDowngrade(opCtx, requestedVersion);
        }

        return true;
    }

} setFeatureCompatibilityVersionCommand;

}  // namespace
}  // namespace mongo
