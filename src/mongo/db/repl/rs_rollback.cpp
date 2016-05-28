/* @file rs_rollback.cpp
*
*    Copyright (C) 2008-2014 MongoDB Inc.
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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/rs_rollback.h"

#include <algorithm>
#include <memory>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_interface.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/roll_back_local_operations.h"
#include "mongo/db/repl/rollback_source.h"
#include "mongo/db/repl/rslog.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/util/log.h"

/* Scenarios
 *
 * We went offline with ops not replicated out.
 *
 *     F = node that failed and coming back.
 *     P = node that took over, new primary
 *
 * #1:
 *     F : a b c d e f g
 *     P : a b c d q
 *
 * The design is "keep P".  One could argue here that "keep F" has some merits, however, in most
 * cases P will have significantly more data.  Also note that P may have a proper subset of F's
 * stream if there were no subsequent writes.
 *
 * For now the model is simply : get F back in sync with P.  If P was really behind or something, we
 * should have just chosen not to fail over anyway.
 *
 * #2:
 *     F : a b c d e f g                -> a b c d
 *     P : a b c d
 *
 * #3:
 *     F : a b c d e f g                -> a b c d q r s t u v w x z
 *     P : a b c d.q r s t u v w x z
 *
 * Steps
 *  find an event in common. 'd'.
 *  undo our events beyond that by:
 *    (1) taking copy from other server of those objects
 *    (2) do not consider copy valid until we pass reach an optime after when we fetched the new
 *        version of object
 *        -- i.e., reset minvalid.
 *    (3) we could skip operations on objects that are previous in time to our capture of the object
 *        as an optimization.
 *
 */

namespace mongo {

using std::shared_ptr;
using std::unique_ptr;
using std::endl;
using std::list;
using std::map;
using std::multimap;
using std::set;
using std::string;
using std::pair;

namespace repl {
namespace {

class RSFatalException : public std::exception {
public:
    RSFatalException(std::string m = "replica set fatal exception") : msg(m) {}
    virtual ~RSFatalException() throw(){};
    virtual const char* what() const throw() {
        return msg.c_str();
    }

private:
    std::string msg;
};

struct DocID {
    // ns and _id both point into ownedObj's buffer
    BSONObj ownedObj;
    const char* ns;
    BSONElement _id;
    bool operator<(const DocID& other) const {
        int comp = strcmp(ns, other.ns);
        if (comp < 0)
            return true;
        if (comp > 0)
            return false;
        return _id < other._id;
    }
};

struct FixUpInfo {
    // note this is a set -- if there are many $inc's on a single document we need to rollback,
    // we only need to refetch it once.
    set<DocID> toRefetch;

    // collections to drop
    set<string> toDrop;

    // Indexes to drop.
    // Key is collection namespace. Value is name of index to drop.
    multimap<string, string> indexesToDrop;

    set<string> collectionsToResyncData;
    set<string> collectionsToResyncMetadata;

    Timestamp commonPoint;
    RecordId commonPointOurDiskloc;

    int rbid;  // remote server's current rollback sequence #
};


Status refetch(FixUpInfo& fixUpInfo, const BSONObj& ourObj) {
    const char* op = ourObj.getStringField("op");
    if (*op == 'n')
        return Status::OK();

    if (ourObj.objsize() > 512 * 1024 * 1024)
        throw RSFatalException("rollback too large");

    DocID doc;
    doc.ownedObj = ourObj.getOwned();
    doc.ns = doc.ownedObj.getStringField("ns");
    if (*doc.ns == '\0') {
        warning() << "ignoring op on rollback no ns TODO : " << doc.ownedObj.toString();
        return Status::OK();
    }

    BSONObj obj = doc.ownedObj.getObjectField(*op == 'u' ? "o2" : "o");
    if (obj.isEmpty()) {
        warning() << "ignoring op on rollback : " << doc.ownedObj.toString();
        return Status::OK();
    }

    if (*op == 'c') {
        BSONElement first = obj.firstElement();
        NamespaceString nss(doc.ns);  // foo.$cmd
        string cmdname = first.fieldName();
        Command* cmd = Command::findCommand(cmdname.c_str());
        if (cmd == NULL) {
            severe() << "rollback no such command " << first.fieldName();
            return Status(ErrorCodes::UnrecoverableRollbackError,
                          str::stream() << "rollback no such command " << first.fieldName(),
                          18751);
        }
        if (cmdname == "create") {
            // Create collection operation
            // { ts: ..., h: ..., op: "c", ns: "foo.$cmd", o: { create: "abc", ... } }
            string ns = nss.db().toString() + '.' + obj["create"].String();  // -> foo.abc
            fixUpInfo.toDrop.insert(ns);
            return Status::OK();
        } else if (cmdname == "drop") {
            string ns = nss.db().toString() + '.' + first.valuestr();
            fixUpInfo.collectionsToResyncData.insert(ns);
            return Status::OK();
        } else if (cmdname == "dropIndexes" || cmdname == "deleteIndexes") {
            // TODO: this is bad.  we simply full resync the collection here,
            //       which could be very slow.
            warning() << "rollback of dropIndexes is slow in this version of "
                      << "mongod";
            string ns = nss.db().toString() + '.' + first.valuestr();
            fixUpInfo.collectionsToResyncData.insert(ns);
            return Status::OK();
        } else if (cmdname == "renameCollection") {
            // TODO: slow.
            warning() << "rollback of renameCollection is slow in this version of "
                      << "mongod";
            string from = first.valuestr();
            string to = obj["to"].String();
            fixUpInfo.collectionsToResyncData.insert(from);
            fixUpInfo.collectionsToResyncData.insert(to);
            return Status::OK();
        } else if (cmdname == "dropDatabase") {
            severe() << "rollback : can't rollback drop database full resync "
                     << "will be required";
            log() << obj.toString();
            throw RSFatalException();
        } else if (cmdname == "collMod") {
            const auto ns = NamespaceString(cmd->parseNs(nss.db().toString(), obj));
            for (auto field : obj) {
                const auto modification = field.fieldNameStringData();
                if (modification == cmdname) {
                    continue;  // Skipping command name.
                }

                if (modification == "validator" || modification == "validationAction" ||
                    modification == "validationLevel" || modification == "usePowerOf2Sizes" ||
                    modification == "noPadding") {
                    fixUpInfo.collectionsToResyncMetadata.insert(ns.ns());
                    continue;
                }

                severe() << "cannot rollback a collMod command: " << obj;
                throw RSFatalException();
            }
            return Status::OK();
        } else if (cmdname == "applyOps") {
            if (first.type() != Array) {
                std::string message = str::stream()
                    << "Expected applyOps argument to be an array; found " << first.toString();
                severe() << message;
                return Status(ErrorCodes::UnrecoverableRollbackError, message);
            }
            for (const auto& subopElement : first.Array()) {
                if (subopElement.type() != Object) {
                    std::string message = str::stream()
                        << "Expected applyOps operations to be of Object type, but found "
                        << subopElement.toString();
                    severe() << message;
                    return Status(ErrorCodes::UnrecoverableRollbackError, message);
                }
                auto subStatus = refetch(fixUpInfo, subopElement.Obj());
                if (!subStatus.isOK()) {
                    return subStatus;
                }
            }
            return Status::OK();
        } else {
            severe() << "can't rollback this command yet: " << obj.toString();
            log() << "cmdname=" << cmdname;
            throw RSFatalException();
        }
    }

    NamespaceString nss(doc.ns);
    if (nss.isSystemDotIndexes()) {
        if (*op != 'i') {
            severe() << "Unexpected operation type '" << *op << "' on system.indexes operation, "
                     << "document: " << doc.ownedObj;
            throw RSFatalException();
        }
        string objNs;
        auto status = bsonExtractStringField(obj, "ns", &objNs);
        if (!status.isOK()) {
            severe() << "Missing collection namespace in system.indexes operation, document: "
                     << doc.ownedObj;
            throw RSFatalException();
        }
        NamespaceString objNss(objNs);
        if (!objNss.isValid()) {
            severe() << "Invalid collection namespace in system.indexes operation, document: "
                     << doc.ownedObj;
            throw RSFatalException();
        }
        string indexName;
        status = bsonExtractStringField(obj, "name", &indexName);
        if (!status.isOK()) {
            severe() << "Missing index name in system.indexes operation, document: "
                     << doc.ownedObj;
            throw RSFatalException();
        }
        using ValueType = multimap<string, string>::value_type;
        ValueType pairToInsert = std::make_pair(objNs, indexName);
        auto lowerBound = fixUpInfo.indexesToDrop.lower_bound(objNs);
        auto upperBound = fixUpInfo.indexesToDrop.upper_bound(objNs);
        auto matcher = [pairToInsert](const ValueType& pair) { return pair == pairToInsert; };
        if (!std::count_if(lowerBound, upperBound, matcher)) {
            fixUpInfo.indexesToDrop.insert(pairToInsert);
        }
        return Status::OK();
    }

    doc._id = obj["_id"];
    if (doc._id.eoo()) {
        severe() << "cannot rollback op with no _id. ns: " << doc.ns
                 << ", document: " << doc.ownedObj;
        throw RSFatalException();
    }

    fixUpInfo.toRefetch.insert(doc);
    return Status::OK();
}


void syncFixUp(OperationContext* txn,
               FixUpInfo& fixUpInfo,
               const RollbackSource& rollbackSource,
               ReplicationCoordinator* replCoord) {
    // fetch all first so we needn't handle interruption in a fancy way

    unsigned long long totalSize = 0;

    // namespace -> doc id -> doc
    map<string, map<DocID, BSONObj>> goodVersions;

    BSONObj newMinValid;

    // fetch all the goodVersions of each document from current primary
    DocID doc;
    unsigned long long numFetched = 0;
    try {
        for (set<DocID>::iterator it = fixUpInfo.toRefetch.begin(); it != fixUpInfo.toRefetch.end();
             it++) {
            doc = *it;

            verify(!doc._id.eoo());

            {
                // TODO : slow.  lots of round trips.
                numFetched++;
                BSONObj good = rollbackSource.findOne(NamespaceString(doc.ns), doc._id.wrap());
                totalSize += good.objsize();
                uassert(13410, "replSet too much data to roll back", totalSize < 300 * 1024 * 1024);

                // note good might be eoo, indicating we should delete it
                goodVersions[doc.ns][doc] = good;
            }
        }
        newMinValid = rollbackSource.getLastOperation();
        if (newMinValid.isEmpty()) {
            error() << "rollback error newMinValid empty?";
            return;
        }
    } catch (const DBException& e) {
        LOG(1) << "rollback re-get objects: " << e.toString();
        error() << "rollback couldn't re-get ns:" << doc.ns << " _id:" << doc._id << ' '
                << numFetched << '/' << fixUpInfo.toRefetch.size();
        throw e;
    }

    log() << "rollback 3.5";
    if (fixUpInfo.rbid != rollbackSource.getRollbackId()) {
        // Our source rolled back itself so the data we received isn't necessarily consistent.
        warning() << "rollback rbid on source changed during rollback, "
                  << "cancelling this attempt";
        return;
    }

    // update them
    log() << "rollback 4 n:" << goodVersions.size();

    bool warn = false;

    invariant(!fixUpInfo.commonPointOurDiskloc.isNull());

    // we have items we are writing that aren't from a point-in-time.  thus best not to come
    // online until we get to that point in freshness.
    OpTime minValid = fassertStatusOK(28774, OpTime::parseFromOplogEntry(newMinValid));
    log() << "minvalid=" << minValid;
    StorageInterface::get(txn)->setMinValid(txn, {OpTime{}, minValid});

    // any full collection resyncs required?
    if (!fixUpInfo.collectionsToResyncData.empty() ||
        !fixUpInfo.collectionsToResyncMetadata.empty()) {
        for (const string& ns : fixUpInfo.collectionsToResyncData) {
            log() << "rollback 4.1.1 coll resync " << ns;

            fixUpInfo.indexesToDrop.erase(ns);
            fixUpInfo.collectionsToResyncMetadata.erase(ns);

            const NamespaceString nss(ns);


            {
                ScopedTransaction transaction(txn, MODE_IX);
                Lock::DBLock dbLock(txn->lockState(), nss.db(), MODE_X);
                Database* db = dbHolder().openDb(txn, nss.db().toString());
                invariant(db);
                WriteUnitOfWork wunit(txn);
                db->dropCollection(txn, ns);
                wunit.commit();
            }

            rollbackSource.copyCollectionFromRemote(txn, nss);
        }

        for (const string& ns : fixUpInfo.collectionsToResyncMetadata) {
            log() << "rollback 4.1.2 coll metadata resync " << ns;

            const NamespaceString nss(ns);
            ScopedTransaction transaction(txn, MODE_IX);
            Lock::DBLock dbLock(txn->lockState(), nss.db(), MODE_X);
            auto db = dbHolder().openDb(txn, nss.db().toString());
            invariant(db);
            auto collection = db->getCollection(ns);
            invariant(collection);
            auto cce = collection->getCatalogEntry();

            auto infoResult = rollbackSource.getCollectionInfo(nss);

            if (!infoResult.isOK()) {
                // Collection dropped by "them" so we should drop it too.
                log() << ns << " not found on remote host, dropping";
                fixUpInfo.toDrop.insert(ns);
                continue;
            }

            auto info = infoResult.getValue();
            CollectionOptions options;
            if (auto optionsField = info["options"]) {
                if (optionsField.type() != Object) {
                    throw RSFatalException(str::stream() << "Failed to parse options " << info
                                                         << ": expected 'options' to be an "
                                                         << "Object, got "
                                                         << typeName(optionsField.type()));
                }

                auto status = options.parse(optionsField.Obj());
                if (!status.isOK()) {
                    throw RSFatalException(str::stream() << "Failed to parse options " << info
                                                         << ": "
                                                         << status.toString());
                }
            } else {
                // Use default options.
            }

            WriteUnitOfWork wuow(txn);
            if (options.flagsSet || cce->getCollectionOptions(txn).flagsSet) {
                cce->updateFlags(txn, options.flags);
            }

            auto status = collection->setValidator(txn, options.validator);
            if (!status.isOK()) {
                throw RSFatalException(str::stream() << "Failed to set validator: "
                                                     << status.toString());
            }
            status = collection->setValidationAction(txn, options.validationAction);
            if (!status.isOK()) {
                throw RSFatalException(str::stream() << "Failed to set validationAction: "
                                                     << status.toString());
            }

            status = collection->setValidationLevel(txn, options.validationLevel);
            if (!status.isOK()) {
                throw RSFatalException(str::stream() << "Failed to set validationLevel: "
                                                     << status.toString());
            }

            wuow.commit();
        }

        // we did more reading from primary, so check it again for a rollback (which would mess
        // us up), and make minValid newer.
        log() << "rollback 4.2";

        string err;
        try {
            newMinValid = rollbackSource.getLastOperation();
            if (newMinValid.isEmpty()) {
                err = "can't get minvalid from sync source";
            } else {
                OpTime minValid = fassertStatusOK(28775, OpTime::parseFromOplogEntry(newMinValid));
                log() << "minvalid=" << minValid;
                const OpTime start{fixUpInfo.commonPoint, OpTime::kUninitializedTerm};
                StorageInterface::get(txn)->setMinValid(txn, {start, minValid});
            }
        } catch (const DBException& e) {
            err = "can't get/set minvalid: ";
            err += e.what();
        }
        if (fixUpInfo.rbid != rollbackSource.getRollbackId()) {
            // our source rolled back itself.  so the data we received isn't necessarily
            // consistent. however, we've now done writes.  thus we have a problem.
            err += "rbid at primary changed during resync/rollback";
        }
        if (!err.empty()) {
            severe() << "rolling back : " << err << ". A full resync will be necessary.";
            // TODO: reset minvalid so that we are permanently in fatal state
            // TODO: don't be fatal, but rather, get all the data first.
            throw RSFatalException();
        }
        log() << "rollback 4.3";
    }

    log() << "rollback 4.6";
    // drop collections to drop before doing individual fixups - that might make things faster
    // below actually if there were subsequent inserts to rollback
    for (set<string>::iterator it = fixUpInfo.toDrop.begin(); it != fixUpInfo.toDrop.end(); it++) {
        log() << "rollback drop: " << *it;

        fixUpInfo.indexesToDrop.erase(*it);

        ScopedTransaction transaction(txn, MODE_IX);
        const NamespaceString nss(*it);
        Lock::DBLock dbLock(txn->lockState(), nss.db(), MODE_X);
        Database* db = dbHolder().get(txn, nsToDatabaseSubstring(*it));
        if (db) {
            WriteUnitOfWork wunit(txn);

            Helpers::RemoveSaver removeSaver("rollback", "", *it);

            // perform a collection scan and write all documents in the collection to disk
            std::unique_ptr<PlanExecutor> exec(InternalPlanner::collectionScan(
                txn, *it, db->getCollection(*it), PlanExecutor::YIELD_MANUAL));
            BSONObj curObj;
            PlanExecutor::ExecState execState;
            while (PlanExecutor::ADVANCED == (execState = exec->getNext(&curObj, NULL))) {
                auto status = removeSaver.goingToDelete(curObj);
                if (!status.isOK()) {
                    severe() << "rolling back createCollection on " << *it
                             << " failed to write document to remove saver file: " << status;
                    throw RSFatalException();
                }
            }
            if (execState != PlanExecutor::IS_EOF) {
                if (execState == PlanExecutor::FAILURE &&
                    WorkingSetCommon::isValidStatusMemberObject(curObj)) {
                    Status errorStatus = WorkingSetCommon::getMemberObjectStatus(curObj);
                    severe() << "rolling back createCollection on " << *it << " failed with "
                             << errorStatus << ". A full resync is necessary.";
                } else {
                    severe() << "rolling back createCollection on " << *it
                             << " failed. A full resync is necessary.";
                }

                throw RSFatalException();
            }

            db->dropCollection(txn, *it);
            wunit.commit();
        }
    }

    // Drop indexes.
    for (auto it = fixUpInfo.indexesToDrop.begin(); it != fixUpInfo.indexesToDrop.end(); it++) {
        const NamespaceString nss(it->first);
        const string& indexName = it->second;
        log() << "rollback drop index: collection: " << nss.toString() << ". index: " << indexName;

        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dbLock(txn->lockState(), nss.db(), MODE_X);
        auto db = dbHolder().get(txn, nss.db());
        if (!db) {
            continue;
        }
        auto collection = db->getCollection(nss.toString());
        if (!collection) {
            continue;
        }
        auto indexCatalog = collection->getIndexCatalog();
        if (!indexCatalog) {
            continue;
        }
        bool includeUnfinishedIndexes = false;
        auto indexDescriptor =
            indexCatalog->findIndexByName(txn, indexName, includeUnfinishedIndexes);
        if (!indexDescriptor) {
            warning() << "rollback failed to drop index " << indexName << " in " << nss.toString()
                      << ": index not found";
            continue;
        }
        WriteUnitOfWork wunit(txn);
        auto status = indexCatalog->dropIndex(txn, indexDescriptor);
        if (!status.isOK()) {
            severe() << "rollback failed to drop index " << indexName << " in " << nss.toString()
                     << ": " << status;
            throw RSFatalException();
        }
        wunit.commit();
    }

    log() << "rollback 4.7";
    unsigned deletes = 0, updates = 0;
    time_t lastProgressUpdate = time(0);
    time_t progressUpdateGap = 10;
    for (const auto& nsAndGoodVersionsByDocID : goodVersions) {
        // Keep an archive of items rolled back if the collection has not been dropped
        // while rolling back createCollection operations.
        const auto& ns = nsAndGoodVersionsByDocID.first;
        unique_ptr<Helpers::RemoveSaver> removeSaver;
        if (!fixUpInfo.toDrop.count(ns)) {
            removeSaver.reset(new Helpers::RemoveSaver("rollback", "", ns));
        }

        const auto& goodVersionsByDocID = nsAndGoodVersionsByDocID.second;
        for (const auto& idAndDoc : goodVersionsByDocID) {
            time_t now = time(0);
            if (now - lastProgressUpdate > progressUpdateGap) {
                log() << deletes << " delete and " << updates
                      << " update operations processed out of " << goodVersions.size()
                      << " total operations";
                lastProgressUpdate = now;
            }
            const DocID& doc = idAndDoc.first;
            BSONObj pattern = doc._id.wrap();  // { _id : ... }
            try {
                verify(doc.ns && *doc.ns);
                if (fixUpInfo.collectionsToResyncData.count(doc.ns)) {
                    // We just synced this entire collection.
                    continue;
                }

                // TODO: Lots of overhead in context. This can be faster.
                const NamespaceString docNss(doc.ns);
                ScopedTransaction transaction(txn, MODE_IX);
                Lock::DBLock docDbLock(txn->lockState(), docNss.db(), MODE_X);
                OldClientContext ctx(txn, doc.ns);

                Collection* collection = ctx.db()->getCollection(doc.ns);

                // Add the doc to our rollback file if the collection was not dropped while
                // rolling back createCollection operations.
                // Do not log an error when undoing an insert on a no longer existent
                // collection.
                // It is likely that the collection was dropped as part of rolling back a
                // createCollection command and regardless, the document no longer exists.
                if (collection && removeSaver) {
                    BSONObj obj;
                    bool found = Helpers::findOne(txn, collection, pattern, obj, false);
                    if (found) {
                        auto status = removeSaver->goingToDelete(obj);
                        if (!status.isOK()) {
                            severe() << "rollback cannot write document in namespace " << doc.ns
                                     << " to archive file: " << status;
                            throw RSFatalException();
                        }
                    } else {
                        error() << "rollback cannot find object: " << pattern << " in namespace "
                                << doc.ns;
                    }
                }

                if (idAndDoc.second.isEmpty()) {
                    // wasn't on the primary; delete.
                    // TODO 1.6 : can't delete from a capped collection.  need to handle that
                    // here.
                    deletes++;

                    if (collection) {
                        if (collection->isCapped()) {
                            // can't delete from a capped collection - so we truncate instead.
                            // if
                            // this item must go, so must all successors!!!
                            try {
                                // TODO: IIRC cappedTruncateAfter does not handle completely
                                // empty.
                                // this will crazy slow if no _id index.
                                const auto clock = txn->getServiceContext()->getFastClockSource();
                                const auto findOneStart = clock->now();
                                RecordId loc = Helpers::findOne(txn, collection, pattern, false);
                                if (clock->now() - findOneStart > Milliseconds(200))
                                    warning() << "roll back slow no _id index for " << doc.ns
                                              << " perhaps?";
                                // would be faster but requires index:
                                // RecordId loc = Helpers::findById(nsd, pattern);
                                if (!loc.isNull()) {
                                    try {
                                        collection->temp_cappedTruncateAfter(txn, loc, true);
                                    } catch (const DBException& e) {
                                        if (e.getCode() == 13415) {
                                            // hack: need to just make cappedTruncate do this...
                                            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                                                WriteUnitOfWork wunit(txn);
                                                uassertStatusOK(collection->truncate(txn));
                                                wunit.commit();
                                            }
                                            MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
                                                txn, "truncate", collection->ns().ns());
                                        } else {
                                            throw e;
                                        }
                                    }
                                }
                            } catch (const DBException& e) {
                                error() << "rolling back capped collection rec " << doc.ns << ' '
                                        << e.toString();
                            }
                        } else {
                            deleteObjects(txn,
                                          collection,
                                          doc.ns,
                                          pattern,
                                          PlanExecutor::YIELD_MANUAL,
                                          true,   // justone
                                          true);  // god
                        }
                        // did we just empty the collection?  if so let's check if it even
                        // exists on the source.
                        if (collection->numRecords(txn) == 0) {
                            try {
                                NamespaceString nss(doc.ns);
                                auto infoResult = rollbackSource.getCollectionInfo(nss);
                                if (!infoResult.isOK()) {
                                    // we should drop
                                    WriteUnitOfWork wunit(txn);
                                    ctx.db()->dropCollection(txn, doc.ns);
                                    wunit.commit();
                                }
                            } catch (const DBException& ex) {
                                // Failed to run listCollections command on sync source.
                                // This isn't *that* big a deal, but is bad.
                                warning() << "rollback error querying for existence of " << doc.ns
                                          << " at the primary, ignoring: " << ex;
                            }
                        }
                    }
                } else {
                    // TODO faster...
                    updates++;

                    const NamespaceString requestNs(doc.ns);
                    UpdateRequest request(requestNs);

                    request.setQuery(pattern);
                    request.setUpdates(idAndDoc.second);
                    request.setGod();
                    request.setUpsert();
                    UpdateLifecycleImpl updateLifecycle(requestNs);
                    request.setLifecycle(&updateLifecycle);

                    update(txn, ctx.db(), request);
                }
            } catch (const DBException& e) {
                log() << "exception in rollback ns:" << doc.ns << ' ' << pattern.toString() << ' '
                      << e.toString() << " ndeletes:" << deletes;
                warn = true;
            }
        }
    }

    log() << "rollback 5 d:" << deletes << " u:" << updates;
    log() << "rollback 6";

    // clean up oplog
    LOG(2) << "rollback truncate oplog after " << fixUpInfo.commonPoint.toStringPretty();
    {
        const NamespaceString oplogNss(rsOplogName);
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock oplogDbLock(txn->lockState(), oplogNss.db(), MODE_IX);
        Lock::CollectionLock oplogCollectionLoc(txn->lockState(), oplogNss.ns(), MODE_X);
        OldClientContext ctx(txn, rsOplogName);
        Collection* oplogCollection = ctx.db()->getCollection(rsOplogName);
        if (!oplogCollection) {
            fassertFailedWithStatusNoTrace(13423,
                                           Status(ErrorCodes::UnrecoverableRollbackError,
                                                  str::stream() << "Can't find " << rsOplogName));
        }
        // TODO: fatal error if this throws?
        oplogCollection->temp_cappedTruncateAfter(txn, fixUpInfo.commonPointOurDiskloc, false);
    }

    Status status = getGlobalAuthorizationManager()->initialize(txn);
    if (!status.isOK()) {
        warning() << "Failed to reinitialize auth data after rollback: " << status;
        warn = true;
    }

    // Reload the lastAppliedOpTime and lastDurableOpTime value in the replcoord and the
    // lastAppliedHash value in bgsync to reflect our new last op.
    replCoord->resetLastOpTimesFromOplog(txn);

    // done
    if (warn)
        warning() << "issues during syncRollback, see log";
    else
        log() << "rollback done";
}

Status _syncRollback(OperationContext* txn,
                     const OplogInterface& localOplog,
                     const RollbackSource& rollbackSource,
                     ReplicationCoordinator* replCoord,
                     const SleepSecondsFn& sleepSecondsFn) {
    invariant(!txn->lockState()->isLocked());

    log() << "rollback 0";

    /** by doing this, we will not service reads (return an error as we aren't in secondary
     *  state. that perhaps is moot because of the write lock above, but that write lock
     *  probably gets deferred or removed or yielded later anyway.
     *
     *  also, this is better for status reporting - we know what is happening.
     */
    {
        Lock::GlobalWrite globalWrite(txn->lockState());
        if (!replCoord->setFollowerMode(MemberState::RS_ROLLBACK)) {
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "Cannot transition from "
                                        << replCoord->getMemberState().toString()
                                        << " to "
                                        << MemberState(MemberState::RS_ROLLBACK).toString());
        }
    }

    FixUpInfo how;
    log() << "rollback 1";
    how.rbid = rollbackSource.getRollbackId();
    {
        log() << "rollback 2 FindCommonPoint";
        try {
            auto processOperationForFixUp = [&how](const BSONObj& operation) {
                return refetch(how, operation);
            };
            auto res = syncRollBackLocalOperations(
                localOplog, rollbackSource.getOplog(), processOperationForFixUp);
            if (!res.isOK()) {
                const auto status = res.getStatus();
                switch (status.code()) {
                    case ErrorCodes::OplogStartMissing:
                    case ErrorCodes::UnrecoverableRollbackError:
                        sleepSecondsFn(Seconds(1));
                        return status;
                    default:
                        throw RSFatalException(status.toString());
                }
            } else {
                how.commonPoint = res.getValue().first;
                how.commonPointOurDiskloc = res.getValue().second;
            }
        } catch (const RSFatalException& e) {
            error() << string(e.what());
            return Status(ErrorCodes::UnrecoverableRollbackError,
                          str::stream()
                              << "need to rollback, but unable to determine common point between"
                                 " local and remote oplog: "
                              << e.what(),
                          18752);
        } catch (const DBException& e) {
            warning() << "rollback 2 exception " << e.toString() << "; sleeping 1 min";

            sleepSecondsFn(Seconds(60));
            throw;
        }
    }

    log() << "rollback 3 fixup";

    replCoord->incrementRollbackID();
    try {
        syncFixUp(txn, how, rollbackSource, replCoord);
    } catch (const RSFatalException& e) {
        error() << "exception during rollback: " << e.what();
        return Status(ErrorCodes::UnrecoverableRollbackError,
                      str::stream() << "exception during rollback: " << e.what(),
                      18753);
    } catch (...) {
        replCoord->incrementRollbackID();

        throw;
    }
    replCoord->incrementRollbackID();

    // Success; leave "ROLLBACK" state intact until applier thread has reloaded the new minValid.
    // Otherwise, the applier could transition the node to SECONDARY with an out-of-date minValid.
    return Status::OK();
}

}  // namespace

Status syncRollback(OperationContext* txn,
                    const OplogInterface& localOplog,
                    const RollbackSource& rollbackSource,
                    ReplicationCoordinator* replCoord,
                    const SleepSecondsFn& sleepSecondsFn) {
    invariant(txn);
    invariant(replCoord);

    log() << "beginning rollback" << rsLog;

    DisableDocumentValidation validationDisabler(txn);
    txn->setReplicatedWrites(false);
    Status status = _syncRollback(txn, localOplog, rollbackSource, replCoord, sleepSecondsFn);

    log() << "rollback finished" << rsLog;
    return status;
}

Status syncRollback(OperationContext* txn,
                    const OplogInterface& localOplog,
                    const RollbackSource& rollbackSource,
                    ReplicationCoordinator* replCoord) {
    return syncRollback(txn, localOplog, rollbackSource, replCoord, [](Seconds seconds) {
        sleepsecs(durationCount<Seconds>(seconds));
    });
}

}  // namespace repl
}  // namespace mongo
