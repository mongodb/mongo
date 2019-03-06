
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_chunk_cloner_source_legacy.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/write_concern.h"

/**
 * This file contains commands, which are specific to the legacy chunk cloner source.
 */
namespace mongo {
namespace {

/**
 * Shortcut class to perform the appropriate checks and acquire the cloner associated with the
 * currently active migration. Uses the currently registered migration for this shard and ensures
 * the session ids match.
 */
class AutoGetActiveCloner {
    MONGO_DISALLOW_COPYING(AutoGetActiveCloner);

public:
    AutoGetActiveCloner(OperationContext* opCtx, const MigrationSessionId& migrationSessionId) {
        const auto nss = ActiveMigrationsRegistry::get(opCtx).getActiveDonateChunkNss();
        uassert(ErrorCodes::NotYetInitialized, "No active migrations were found", nss);

        // Once the collection is locked, the migration status cannot change
        _autoColl.emplace(opCtx, *nss, MODE_IS);

        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection " << nss->ns() << " does not exist",
                _autoColl->getCollection());

        if (auto msm = MigrationSourceManager::get(CollectionShardingRuntime::get(opCtx, *nss))) {
            // It is now safe to access the cloner
            _chunkCloner = dynamic_cast<MigrationChunkClonerSourceLegacy*>(msm->getCloner());
            invariant(_chunkCloner);

        } else {
            uasserted(ErrorCodes::IllegalOperation,
                      str::stream() << "No active migrations were found for collection "
                                    << nss->ns());
        }

        // Ensure the session ids are correct
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "Requested migration session id " << migrationSessionId.toString()
                              << " does not match active session id "
                              << _chunkCloner->getSessionId().toString(),
                migrationSessionId.matches(_chunkCloner->getSessionId()));
    }

    Database* getDb() const {
        invariant(_autoColl);
        return _autoColl->getDb();
    }

    Collection* getColl() const {
        invariant(_autoColl);
        return _autoColl->getCollection();
    }

    MigrationChunkClonerSourceLegacy* getCloner() const {
        invariant(_chunkCloner);
        return _chunkCloner;
    }

private:
    // Scoped database + collection lock
    boost::optional<AutoGetCollection> _autoColl;

    // Contains the active cloner for the namespace
    MigrationChunkClonerSourceLegacy* _chunkCloner;
};

class InitialCloneCommand : public BasicCommand {
public:
    InitialCloneCommand() : BasicCommand("_migrateClone") {}

    std::string help() const override {
        return "internal";
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const std::string&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        const MigrationSessionId migrationSessionId(
            uassertStatusOK(MigrationSessionId::extractFromBSON(cmdObj)));

        boost::optional<BSONArrayBuilder> arrBuilder;

        // Try to maximize on the size of the buffer, which we are returning in order to have less
        // round-trips
        int arrSizeAtPrevIteration = -1;

        while (!arrBuilder || arrBuilder->arrSize() > arrSizeAtPrevIteration) {
            AutoGetActiveCloner autoCloner(opCtx, migrationSessionId);

            if (!arrBuilder) {
                arrBuilder.emplace(autoCloner.getCloner()->getCloneBatchBufferAllocationSize());
            }

            arrSizeAtPrevIteration = arrBuilder->arrSize();

            uassertStatusOK(autoCloner.getCloner()->nextCloneBatch(
                opCtx, autoCloner.getColl(), arrBuilder.get_ptr()));
        }

        invariant(arrBuilder);
        result.appendArray("objects", arrBuilder->arr());

        return true;
    }

} initialCloneCommand;

class TransferModsCommand : public BasicCommand {
public:
    TransferModsCommand() : BasicCommand("_transferMods") {}

    std::string help() const override {
        return "internal";
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const std::string&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        const MigrationSessionId migrationSessionId(
            uassertStatusOK(MigrationSessionId::extractFromBSON(cmdObj)));

        AutoGetActiveCloner autoCloner(opCtx, migrationSessionId);

        uassertStatusOK(autoCloner.getCloner()->nextModsBatch(opCtx, autoCloner.getDb(), &result));
        return true;
    }

} transferModsCommand;

/**
 * Command for extracting the oplog entries that needs to be migrated for the given migration
 * session id.
 * Note: this command is not stateless. Calling this command has a side-effect of gradually
 * depleting the buffer that contains the oplog entries to be transfered.
 */
class MigrateSessionCommand : public BasicCommand {
public:
    MigrateSessionCommand() : BasicCommand("_getNextSessionMods") {}

    std::string help() const override {
        return "internal";
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    /**
     * Fetches the next batch of oplog that needs to be transferred and appends it to the given
     * array builder. If it was not able to fetch anything, it will return a non-null notification
     * that will get signalled when new batches comes in or when migration is over. If the boolean
     * value from the notification returns true, then the migration has entered the critical
     * section or aborted and there's no more new batches to fetch.
     */
    std::shared_ptr<Notification<bool>> fetchNextSessionMigrationBatch(
        OperationContext* opCtx,
        const MigrationSessionId& migrationSessionId,
        BSONArrayBuilder* arrBuilder) {
        boost::optional<repl::OpTime> opTime;
        std::shared_ptr<Notification<bool>> newOplogNotification;

        writeConflictRetry(
            opCtx,
            "Fetching session related oplogs for migration",
            NamespaceString::kRsOplogNamespace.ns(),
            [&]() {
                AutoGetActiveCloner autoCloner(opCtx, migrationSessionId);
                opTime = autoCloner.getCloner()->nextSessionMigrationBatch(opCtx, arrBuilder);

                if (arrBuilder->arrSize() == 0) {
                    newOplogNotification =
                        autoCloner.getCloner()->getNotificationForNextSessionMigrationBatch();
                }
            });

        if (newOplogNotification) {
            return newOplogNotification;
        }

        // If the batch returns something, we wait for write concern to ensure that all the entries
        // in the batch have been majority committed. We then need to check that the rollback id
        // hasn't changed since we started migration, because a change would indicate that some data
        // in this batch may have been rolled back. In this case, we abort the migration.
        if (opTime) {
            WriteConcernResult wcResult;
            WriteConcernOptions majorityWC(
                WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, 0);
            uassertStatusOK(waitForWriteConcern(opCtx, opTime.get(), majorityWC, &wcResult));

            auto rollbackIdAtMigrationInit = [&]() {
                AutoGetActiveCloner autoCloner(opCtx, migrationSessionId);
                return autoCloner.getCloner()->getRollbackIdAtInit();
            }();

            // The check for rollback id must be done after having waited for majority in order to
            // ensure that whatever was waited on didn't get rolled back.
            auto rollbackId = repl::ReplicationProcess::get(opCtx)->getRollbackID();
            uassert(50881,
                    str::stream() << "rollback detected, rollbackId was "
                                  << rollbackIdAtMigrationInit
                                  << " but is now "
                                  << rollbackId,
                    rollbackId == rollbackIdAtMigrationInit);
        }

        return nullptr;
    }

    bool run(OperationContext* opCtx,
             const std::string&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        const MigrationSessionId migrationSessionId(
            uassertStatusOK(MigrationSessionId::extractFromBSON(cmdObj)));

        BSONArrayBuilder arrBuilder;
        bool hasMigrationCompleted = false;

        do {
            if (auto newOplogNotification =
                    fetchNextSessionMigrationBatch(opCtx, migrationSessionId, &arrBuilder)) {
                hasMigrationCompleted = newOplogNotification->get(opCtx);
            } else if (arrBuilder.arrSize() == 0) {
                // If we didn't get a notification and the arrBuilder is empty, that means
                // that the sessionMigration is not active for this migration (most likely
                // because it's not a replica set).
                hasMigrationCompleted = true;
            }
        } while (arrBuilder.arrSize() == 0 && !hasMigrationCompleted);

        result.appendArray("oplog", arrBuilder.arr());

        // TODO: SERVER-40187 remove after v4.2. This is to indicate the caller that this server
        // waits for notification on new oplog entries to send over so the caller doesn't need
        // to throttle.
        result.append("waitsForNewOplog", true);

        return true;
    }

} migrateSessionCommand;

}  // namespace
}  // namespace mongo
