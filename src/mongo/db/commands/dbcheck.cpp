/**
 * Copyright (C) 2013 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/health_log.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/command_generic_argument.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/dbcheck.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/background.h"

#include "mongo/util/log.h"

namespace mongo {

namespace {
constexpr uint64_t kBatchDocs = 5'000;
constexpr uint64_t kBatchBytes = 20'000'000;


/**
 * All the information needed to run dbCheck on a single collection.
 */
struct DbCheckCollectionInfo {
    NamespaceString nss;
    BSONKey start;
    BSONKey end;
    int64_t maxCount;
    int64_t maxSize;
    int64_t maxRate;
};

/**
 * A run of dbCheck consists of a series of collections.
 */
using DbCheckRun = std::vector<DbCheckCollectionInfo>;

/**
 * Check if dbCheck can run on the given namespace.
 */
bool canRunDbCheckOn(const NamespaceString& nss) {
    if (nss.isLocal()) {
        return false;
    }

    // TODO: SERVER-30826.
    const std::set<StringData> replicatedSystemCollections{"system.backup_users",
                                                           "system.js",
                                                           "system.new_users",
                                                           "system.roles",
                                                           "system.users",
                                                           "system.version",
                                                           "system.views"};
    if (nss.isSystem()) {
        if (replicatedSystemCollections.count(nss.coll()) == 0) {
            return false;
        }
    }

    return true;
}

std::unique_ptr<DbCheckRun> singleCollectionRun(OperationContext* opCtx,
                                                const std::string& dbName,
                                                const DbCheckSingleInvocation& invocation) {
    NamespaceString nss(dbName, invocation.getColl());
    AutoGetCollectionForRead agc(opCtx, nss);

    uassert(ErrorCodes::NamespaceNotFound,
            "Collection " + invocation.getColl() + " not found",
            agc.getCollection());

    uassert(40619,
            "Cannot run dbCheck on " + nss.toString() + " because it is not replicated",
            canRunDbCheckOn(nss));

    auto start = invocation.getMinKey();
    auto end = invocation.getMaxKey();
    auto maxCount = invocation.getMaxCount();
    auto maxSize = invocation.getMaxSize();
    auto maxRate = invocation.getMaxCountPerSecond();
    auto info = DbCheckCollectionInfo{nss, start, end, maxCount, maxSize, maxRate};
    auto result = stdx::make_unique<DbCheckRun>();
    result->push_back(info);
    return result;
}

std::unique_ptr<DbCheckRun> fullDatabaseRun(OperationContext* opCtx,
                                            const std::string& dbName,
                                            const DbCheckAllInvocation& invocation) {
    uassert(
        ErrorCodes::InvalidNamespace, "Cannot run dbCheck on local database", dbName != "local");

    // Read the list of collections in a database-level lock.
    AutoGetDb agd(opCtx, StringData(dbName), MODE_S);
    auto db = agd.getDb();
    auto result = stdx::make_unique<DbCheckRun>();

    uassert(ErrorCodes::NamespaceNotFound, "Database " + dbName + " not found", agd.getDb());

    int64_t max = std::numeric_limits<int64_t>::max();
    auto rate = invocation.getMaxCountPerSecond();

    for (Collection* coll : *db) {
        DbCheckCollectionInfo info{coll->ns(), BSONKey::min(), BSONKey::max(), max, max, rate};
        result->push_back(info);
    }

    return result;
}


/**
 * Factory function for producing DbCheckRun's from command objects.
 */
std::unique_ptr<DbCheckRun> getRun(OperationContext* opCtx,
                                   const std::string& dbName,
                                   const BSONObj& obj) {
    BSONObjBuilder builder;

    // Get rid of generic command fields.
    for (const auto& elem : obj) {
        if (!isGenericArgument(elem.fieldNameStringData())) {
            builder.append(elem);
        }
    }

    BSONObj toParse = builder.obj();

    // If the dbCheck argument is a string, this is the per-collection form.
    if (toParse["dbCheck"].type() == BSONType::String) {
        return singleCollectionRun(
            opCtx, dbName, DbCheckSingleInvocation::parse(IDLParserErrorContext(""), toParse));
    } else {
        // Otherwise, it's the database-wide form.
        return fullDatabaseRun(
            opCtx, dbName, DbCheckAllInvocation::parse(IDLParserErrorContext(""), toParse));
    }
}


/**
 * The BackgroundJob in which dbCheck actually executes on the primary.
 */
class DbCheckJob : public BackgroundJob {
public:
    DbCheckJob(const StringData& dbName, std::unique_ptr<DbCheckRun> run)
        : BackgroundJob(true), _done(false), _dbName(dbName.toString()), _run(std::move(run)) {}

protected:
    virtual std::string name() const override {
        return "dbCheck";
    }

    virtual void run() override {
        // Every dbCheck runs in its own client.
        Client::initThread(name());

        for (const auto& coll : *_run) {
            try {
                _doCollection(coll);
            } catch (const DBException& e) {
                auto logEntry = dbCheckErrorHealthLogEntry(
                    coll.nss, "dbCheck failed", OplogEntriesEnum::Batch, e.toStatus());
                HealthLog::get(Client::getCurrent()->getServiceContext()).log(*logEntry);
                return;
            }

            if (_done) {
                log() << "dbCheck terminated due to stepdown";
                return;
            }
        }
    }

private:
    void _doCollection(const DbCheckCollectionInfo& info) {
        // If we can't find the collection, abort the check.
        if (!_getCollectionMetadata(info)) {
            return;
        }

        if (_done) {
            return;
        }

        // Parameters for the hasher.
        auto start = info.start;
        bool reachedEnd = false;

        // Make sure the totals over all of our batches don't exceed the provided limits.
        int64_t totalBytesSeen = 0;
        int64_t totalDocsSeen = 0;

        // Limit the rate of the check.
        using Clock = stdx::chrono::system_clock;
        using TimePoint = stdx::chrono::time_point<Clock>;
        TimePoint lastStart = Clock::now();
        int64_t docsInCurrentInterval = 0;

        do {
            using namespace std::literals::chrono_literals;

            if (Clock::now() - lastStart > 1s) {
                lastStart = Clock::now();
                docsInCurrentInterval = 0;
            }

            auto result = _runBatch(info, start, kBatchDocs, kBatchBytes);

            if (_done) {
                return;
            }

            std::unique_ptr<HealthLogEntry> entry;

            if (!result.isOK()) {
                entry = dbCheckErrorHealthLogEntry(
                    info.nss, "dbCheck batch failed", OplogEntriesEnum::Batch, result.getStatus());
                HealthLog::get(Client::getCurrent()->getServiceContext()).log(*entry);
                return;
            } else {
                auto stats = result.getValue();
                entry = dbCheckBatchEntry(info.nss,
                                          stats.nDocs,
                                          stats.nBytes,
                                          stats.md5,
                                          stats.md5,
                                          start,
                                          stats.lastKey,
                                          stats.time);
                HealthLog::get(Client::getCurrent()->getServiceContext()).log(*entry);
            }

            auto stats = result.getValue();

            start = stats.lastKey;

            // Update our running totals.
            totalDocsSeen += stats.nDocs;
            totalBytesSeen += stats.nBytes;
            docsInCurrentInterval += stats.nDocs;

            // Check if we've exceeded any limits.
            bool reachedLast = stats.lastKey >= info.end;
            bool tooManyDocs = totalDocsSeen >= info.maxCount;
            bool tooManyBytes = totalBytesSeen >= info.maxSize;
            reachedEnd = reachedLast || tooManyDocs || tooManyBytes;

            if (docsInCurrentInterval > info.maxRate && info.maxRate > 0) {
                // If an extremely low max rate has been set (substantially smaller than the batch
                // size) we might want to sleep for multiple seconds between batches.
                int64_t timesExceeded = docsInCurrentInterval / info.maxRate;

                stdx::this_thread::sleep_for(timesExceeded * 1s - (Clock::now() - lastStart));
            }
        } while (!reachedEnd);
    }

    /**
     * For organizing the results of batches.
     */
    struct BatchStats {
        int64_t nDocs;
        int64_t nBytes;
        BSONKey lastKey;
        std::string md5;
        repl::OpTime time;
    };

    // Set if the job cannot proceed.
    bool _done;
    std::string _dbName;
    std::unique_ptr<DbCheckRun> _run;

    bool _getCollectionMetadata(const DbCheckCollectionInfo& info) {
        auto uniqueOpCtx = Client::getCurrent()->makeOperationContext();
        auto opCtx = uniqueOpCtx.get();

        // While we get the prev/next UUID information, we need a database-level lock, plus a global
        // IX lock (see SERVER-28544).
        AutoGetDbForDbCheck agd(opCtx, info.nss);

        if (_stepdownHasOccurred(opCtx, info.nss)) {
            _done = true;
            return true;
        }

        auto db = agd.getDb();
        if (!db) {
            return false;
        }

        auto collection = db->getCollection(opCtx, info.nss);
        if (!collection) {
            return false;
        }

        auto uuid = collection->uuid();
        // Check if UUID exists.
        if (!uuid) {
            return false;
        }
        auto prev = UUIDCatalog::get(opCtx).prev(_dbName, *uuid);
        auto next = UUIDCatalog::get(opCtx).next(_dbName, *uuid);

        // Find and report collection metadata.
        auto indices = collectionIndexInfo(opCtx, collection);
        auto options = collectionOptions(opCtx, collection);

        DbCheckOplogCollection entry;
        entry.setNss(collection->ns());
        entry.setUuid(*collection->uuid());
        if (prev) {
            entry.setPrev(*prev);
        }
        if (next) {
            entry.setNext(*next);
        }
        entry.setType(OplogEntriesEnum::Collection);
        entry.setIndexes(indices);
        entry.setOptions(options);

        // Send information on this collection over the oplog for the secondary to check.
        auto optime = _logOp(opCtx, collection->ns(), collection->uuid(), entry.toBSON());

        DbCheckCollectionInformation collectionInfo;
        collectionInfo.collectionName = collection->ns().coll().toString();
        collectionInfo.prev = entry.getPrev();
        collectionInfo.next = entry.getNext();
        collectionInfo.indexes = entry.getIndexes();
        collectionInfo.options = entry.getOptions();

        auto hle = dbCheckCollectionEntry(
            collection->ns(), *collection->uuid(), collectionInfo, collectionInfo, optime);

        HealthLog::get(opCtx).log(*hle);

        return true;
    }

    StatusWith<BatchStats> _runBatch(const DbCheckCollectionInfo& info,
                                     const BSONKey& first,
                                     int64_t batchDocs,
                                     int64_t batchBytes) {
        // New OperationContext for each batch.
        auto uniqueOpCtx = Client::getCurrent()->makeOperationContext();
        auto opCtx = uniqueOpCtx.get();
        DbCheckOplogBatch batch;

        // Find the relevant collection.
        AutoGetCollectionForDbCheck agc(opCtx, info.nss, OplogEntriesEnum::Batch);

        if (_stepdownHasOccurred(opCtx, info.nss)) {
            _done = true;
            return Status(ErrorCodes::PrimarySteppedDown, "dbCheck terminated due to stepdown");
        }

        auto collection = agc.getCollection();

        if (!collection) {
            return {ErrorCodes::NamespaceNotFound, "dbCheck collection no longer exists"};
        }

        boost::optional<DbCheckHasher> hasher;
        try {
            hasher.emplace(opCtx,
                           collection,
                           first,
                           info.end,
                           std::min(batchDocs, info.maxCount),
                           std::min(batchBytes, info.maxSize));
        } catch (const DBException& e) {
            return e.toStatus();
        }

        Status status = hasher->hashAll();

        if (!status.isOK()) {
            return status;
        }

        std::string md5 = hasher->total();

        batch.setType(OplogEntriesEnum::Batch);
        batch.setNss(info.nss);
        batch.setMd5(md5);
        batch.setMinKey(first);
        batch.setMaxKey(BSONKey(hasher->lastKey()));

        BatchStats result;

        // Send information on this batch over the oplog.
        result.time = _logOp(opCtx, info.nss, collection->uuid(), batch.toBSON());

        result.nDocs = hasher->docsSeen();
        result.nBytes = hasher->bytesSeen();
        result.lastKey = hasher->lastKey();
        result.md5 = md5;

        return result;
    }

    /**
     * Return `true` iff the primary the check is running on has stepped down.
     */
    bool _stepdownHasOccurred(OperationContext* opCtx, const NamespaceString& nss) {
        Status status = opCtx->checkForInterruptNoAssert();

        if (!status.isOK()) {
            return true;
        }

        auto coord = repl::ReplicationCoordinator::get(opCtx);

        if (!coord->canAcceptWritesFor(opCtx, nss)) {
            return true;
        }

        return false;
    }

    repl::OpTime _logOp(OperationContext* opCtx,
                        const NamespaceString& nss,
                        OptionalCollectionUUID uuid,
                        const BSONObj& obj) {
        return writeConflictRetry(
            opCtx, "dbCheck oplog entry", NamespaceString::kRsOplogNamespace.ns(), [&] {
                auto const clockSource = opCtx->getServiceContext()->getFastClockSource();
                const auto wallClockTime = clockSource->now();

                WriteUnitOfWork uow(opCtx);
                repl::OpTime result = repl::logOp(opCtx,
                                                  "c",
                                                  nss,
                                                  uuid,
                                                  obj,
                                                  nullptr,
                                                  false,
                                                  wallClockTime,
                                                  {},
                                                  kUninitializedStmtId,
                                                  {},
                                                  false /* prepare */);
                uow.commit();
                return result;
            });
    }
};

/**
 * The command, as run on the primary.
 */
class DbCheckCmd : public BasicCommand {
public:
    DbCheckCmd() : BasicCommand("dbCheck") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool adminOnly() const {
        return false;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "Validate replica set consistency.\n"
               "Invoke with { dbCheck: <collection name/uuid>,\n"
               "              minKey: <first key, exclusive>,\n"
               "              maxKey: <last key, inclusive>,\n"
               "              maxCount: <max number of docs>,\n"
               "              maxSize: <max size of docs>,\n"
               "              maxCountPerSecond: <max rate in docs/sec> } "
               "to check a collection.\n"
               "Invoke with {dbCheck: 1} to check all collections in the database.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        // For now, just use `find` permissions.
        const NamespaceString nss(parseNs(dbname, cmdObj));

        // First, check that we can read this collection.
        Status status = AuthorizationSession::get(client)->checkAuthForFind(nss, false);

        if (!status.isOK()) {
            return status;
        }

        // Then check that we can read the health log.
        return AuthorizationSession::get(client)->checkAuthForFind(HealthLog::nss, false);
    }

    virtual bool run(OperationContext* opCtx,
                     const std::string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        auto job = getRun(opCtx, dbname, cmdObj);
        try {
            (new DbCheckJob(dbname, std::move(job)))->go();
        } catch (const DBException& e) {
            result.append("ok", false);
            result.append("err", e.toString());
            return false;
        }
        result.append("ok", true);
        return true;
    }
};

MONGO_REGISTER_TEST_COMMAND(DbCheckCmd);
}  // namespace
}
