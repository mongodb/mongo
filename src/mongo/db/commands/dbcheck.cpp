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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/health_log.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/dbcheck.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/background.h"

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
};

/**
 * A run of dbCheck consists of a series of collections.
 */
using DbCheckRun = std::vector<DbCheckCollectionInfo>;

std::unique_ptr<DbCheckRun> singleCollectionRun(OperationContext* opCtx,
                                                const std::string& dbName,
                                                const DbCheckSingleInvocation& invocation) {
    NamespaceString nss(dbName, invocation.getColl());
    AutoGetCollectionForRead agc(opCtx, nss);
    uassert(ErrorCodes::NamespaceNotFound,
            "Collection " + invocation.getColl() + " not found",
            agc.getCollection());
    auto start = invocation.getMinKey();
    auto end = invocation.getMaxKey();
    auto maxCount = invocation.getMaxCount();
    auto maxSize = invocation.getMaxSize();
    auto info = DbCheckCollectionInfo{nss, start, end, maxCount, maxSize};
    auto result = stdx::make_unique<DbCheckRun>();
    result->push_back(info);
    return result;
}

std::unique_ptr<DbCheckRun> fullDatabaseRun(OperationContext* opCtx,
                                            const std::string& dbName,
                                            const DbCheckAllInvocation& invocation) {
    // Read the list of collections in a database-level lock.
    AutoGetDb agd(opCtx, StringData(dbName), MODE_S);
    auto db = agd.getDb();
    auto result = stdx::make_unique<DbCheckRun>();

    uassert(ErrorCodes::NamespaceNotFound, "Database " + dbName + " not found", agd.getDb());

    int64_t max = std::numeric_limits<int64_t>::max();

    for (Collection* coll : *db) {
        DbCheckCollectionInfo info{coll->ns(), BSONKey::min(), BSONKey::max(), max, max};
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
        if (!Command::isGenericArgument(elem.fieldNameStringData())) {
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
        : BackgroundJob(true), _dbName(dbName.toString()), _run(std::move(run)) {}

protected:
    virtual std::string name() const override {
        return "dbCheck";
    }

    virtual void run() override {
        // Every dbCheck runs in its own client.
        Client::initThread(name());

        for (const auto& coll : *_run) {
            _doCollection(coll);
        }
    }

private:
    void _doCollection(const DbCheckCollectionInfo& info) {
        // If we can't find the collection, abort the check.
        if (!_collectionMetadata(info)) {
            return;
        }

        // Parameters for the hasher.
        auto start = info.start;
        bool reachedEnd = false;

        // Make sure the totals over all of our batches don't exceed the provided limits.
        int64_t totalBytesSeen = 0;
        int64_t totalDocsSeen = 0;

        do {
            auto result = _runBatch(info, start, kBatchDocs, kBatchBytes);

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
                                          info.start,
                                          stats.lastKey,
                                          stats.time);
                HealthLog::get(Client::getCurrent()->getServiceContext()).log(*entry);
            }

            auto stats = result.getValue();

            start = stats.lastKey;

            // Update our running totals.
            totalDocsSeen += stats.nDocs;
            totalBytesSeen += stats.nBytes;

            // Check if we've exceeded any limits.
            bool reachedLast = stats.lastKey >= info.end;
            bool tooManyDocs = totalDocsSeen >= info.maxCount;
            bool tooManyBytes = totalBytesSeen >= info.maxSize;
            reachedEnd = reachedLast || tooManyDocs || tooManyBytes;
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

    std::string _dbName;
    std::unique_ptr<DbCheckRun> _run;

    bool _collectionMetadata(const DbCheckCollectionInfo& info) {
        auto uniqueOpCtx = Client::getCurrent()->makeOperationContext();
        auto opCtx = uniqueOpCtx.get();

        // While we get the prev/next UUID information, we need a database-level lock.
        AutoGetDb agd(opCtx, _dbName, MODE_S);

        auto collection = agd.getDb()->getCollection(opCtx, info.nss);

        if (!collection) {
            return false;
        }

        auto prev = UUIDCatalog::get(opCtx).prev(_dbName, *collection->uuid());
        auto next = UUIDCatalog::get(opCtx).next(_dbName, *collection->uuid());

        // Find and report the indices.
        auto indices = collectionIndexInfo(opCtx, collection);

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

        repl::OpTime optime =
            writeConflictRetry(opCtx, "log dbCheck to oplog", collection->ns().ns(), [&] {
                WriteUnitOfWork uow(opCtx);
                auto optime = repl::logOp(opCtx,
                                          "c",
                                          collection->ns(),
                                          collection->uuid(),
                                          entry.toBSON(),
                                          nullptr,
                                          false,
                                          kUninitializedStmtId,
                                          repl::PreAndPostImageTimestamps());
                uow.commit();
                return optime;
            });

        auto hle = dbCheckCollectionEntry(collection->ns(),
                                          collection->ns().coll().toString(),
                                          *collection->uuid(),
                                          entry.getPrev(),
                                          entry.getPrev(),
                                          entry.getNext(),
                                          entry.getNext(),
                                          entry.getIndexes(),
                                          entry.getIndexes(),
                                          optime);

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

        using Result = StatusWith<BatchStats>;
        auto result = writeConflictRetry(opCtx, "dbCheck batch", info.nss.ns(), [&] {
            WriteUnitOfWork uow(opCtx);

            // Find the relevant collection.
            auto agc = getCollectionForDbCheck(opCtx, info.nss, OplogEntriesEnum::Batch);
            auto collection = agc->getCollection();
            if (!collection) {
                return Result(ErrorCodes::NamespaceNotFound, "dbCheck collection no longer exists");
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
                return Result(e.toStatus());
            }

            Status status = hasher->hashAll();

            if (!status.isOK()) {
                return Result(status);
            }

            std::string md5 = hasher->total();

            batch.setType(OplogEntriesEnum::Batch);
            batch.setNss(info.nss);
            batch.setMd5(md5);
            batch.setMinKey(first);
            batch.setMaxKey(BSONKey(hasher->lastKey()));

            BatchStats result;

            result.time = repl::logOp(opCtx,
                                      "c",
                                      info.nss,
                                      collection->uuid(),
                                      batch.toBSON(),
                                      nullptr,
                                      false,
                                      kUninitializedStmtId,
                                      repl::PreAndPostImageTimestamps());

            uow.commit();

            result.nDocs = hasher->docsSeen();
            result.nBytes = hasher->bytesSeen();
            result.lastKey = hasher->lastKey();
            result.md5 = md5;

            return Result(result);
        });

        return result;
    }
};

/**
 * The command, as run on the primary.
 */
class DbCheckCmd : public BasicCommand {
public:
    DbCheckCmd() : BasicCommand("dbCheck") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return false;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void help(std::stringstream& help) const {
        help << "Validate replica set consistency.\n"
             << "Invoke with { dbCheck: <collection name/uuid>,\n"
             << "              minKey: <first key, exclusive>,\n"
             << "              maxKey: <last key, inclusive>,\n"
             << "              maxCount: <max number of docs>,\n"
             << "              maxSize: <max size of docs>,\n"
             << "              maxCountPerSecond: <max rate in docs/sec> } "
             << "to check a collection.\n"
             << "Invoke with {dbCheck: 1} to check all collections in the database.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        // For now, just use `find` permissions.
        const NamespaceString nss(parseNs(dbname, cmdObj));
        auto hasTerm = cmdObj.hasField("term");
        return AuthorizationSession::get(client)->checkAuthForFind(nss, hasTerm);
    }

    virtual bool run(OperationContext* opCtx,
                     const std::string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        uassert(40614, "dbCheck requires FeatureCompatibilityVersion >= 3.6", _hasCorrectFCV());

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

private:
    bool _hasCorrectFCV(void) {
        const auto fcv = serverGlobalParams.featureCompatibility.version.load();
        return fcv >= ServerGlobalParams::FeatureCompatibility::Version::k36;
    }
};

MONGO_INITIALIZER(RegisterDbCheckCmd)(InitializerContext* context) {
    new DbCheckCmd();
    return Status::OK();
}
}  // namespace
}
