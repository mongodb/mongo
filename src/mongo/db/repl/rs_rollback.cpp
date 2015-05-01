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

#include <boost/shared_ptr.hpp>

#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/cloner.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/minvalid.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_impl.h"
#include "mongo/db/repl/rslog.h"
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

    using boost::shared_ptr;
    using std::auto_ptr;
    using std::endl;
    using std::list;
    using std::map;
    using std::set;
    using std::string;
    using std::pair;

namespace repl {
namespace {

    class RSFatalException : public std::exception {
    public:
        RSFatalException(std::string m = "replica set fatal exception")
            : msg(m) {}
        virtual ~RSFatalException() throw() {};
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

        set<string> collectionsToResyncData;
        set<string> collectionsToResyncMetadata;

        Timestamp commonPoint;
        RecordId commonPointOurDiskloc;

        int rbid; // remote server's current rollback sequence #
    };


    /** helper to get rollback id from another server. */
    int getRBID(DBClientConnection *c) {
        bo info;
        c->simpleCommand("admin", &info, "replSetGetRBID");
        return info["rbid"].numberInt();
    }


    void refetch(FixUpInfo& fixUpInfo, const BSONObj& ourObj) {
        const char* op = ourObj.getStringField("op");
        if (*op == 'n')
            return;

        if (ourObj.objsize() > 512 * 1024 * 1024)
            throw RSFatalException("rollback too large");

        DocID doc;
        doc.ownedObj = ourObj.getOwned();
        doc.ns = doc.ownedObj.getStringField("ns");
        if (*doc.ns == '\0') {
            warning() << "ignoring op on rollback no ns TODO : "
                  << doc.ownedObj.toString();
            return;
        }

        BSONObj obj = doc.ownedObj.getObjectField(*op=='u' ? "o2" : "o");
        if (obj.isEmpty()) {
            warning() << "ignoring op on rollback : " << doc.ownedObj.toString();
            return;
        }

        if (*op == 'c') {
            BSONElement first = obj.firstElement();
            NamespaceString nss(doc.ns); // foo.$cmd
            string cmdname = first.fieldName();
            Command* cmd = Command::findCommand(cmdname.c_str());
            if (cmd == NULL) {
                severe() << "rollback no such command " << first.fieldName();
                fassertFailedNoTrace(18751);
            }
            if (cmdname == "create") {
                // Create collection operation
                // { ts: ..., h: ..., op: "c", ns: "foo.$cmd", o: { create: "abc", ... } }
                string ns = nss.db().toString() + '.' + obj["create"].String(); // -> foo.abc
                fixUpInfo.toDrop.insert(ns);
                return;
            }
            else if (cmdname == "drop") {
                string ns = nss.db().toString() + '.' + first.valuestr();
                fixUpInfo.collectionsToResyncData.insert(ns);
                return;
            }
            else if (cmdname == "dropIndexes" || cmdname == "deleteIndexes") {
                // TODO: this is bad.  we simply full resync the collection here,
                //       which could be very slow.
                warning() << "rollback of dropIndexes is slow in this version of "
                          << "mongod";
                string ns = nss.db().toString() + '.' + first.valuestr();
                fixUpInfo.collectionsToResyncData.insert(ns);
                return;
            }
            else if (cmdname == "renameCollection") {
                // TODO: slow.
                warning() << "rollback of renameCollection is slow in this version of "
                          << "mongod";
                string from = first.valuestr();
                string to = obj["to"].String();
                fixUpInfo.collectionsToResyncData.insert(from);
                fixUpInfo.collectionsToResyncData.insert(to);
                return;
            }
            else if (cmdname == "dropDatabase") {
                severe() << "rollback : can't rollback drop database full resync "
                         << "will be required";
                log() << obj.toString();
                throw RSFatalException();
            }
            else if (cmdname == "collMod") {
                const auto ns = NamespaceString(cmd->parseNs(nss.db().toString(), obj));
                for (auto field : obj) {
                    const auto modification = field.fieldNameStringData();
                    if (modification == cmdname) {
                        continue; // Skipping command name.
                    }

                    if (modification == "validator"
                            || modification == "usePowerOf2Sizes"
                            || modification == "noPadding") {
                        fixUpInfo.collectionsToResyncMetadata.insert(ns);
                        continue;
                    }

                    severe() << "cannot rollback a collMod command: " << obj;
                    throw RSFatalException();
                }
            }
            else {
                severe() << "can't rollback this command yet: "
                         << obj.toString();
                log() << "cmdname=" << cmdname;
                throw RSFatalException();
            }
        }

        doc._id = obj["_id"];
        if (doc._id.eoo()) {
            warning() << "ignoring op on rollback no _id TODO : " << doc.ns << ' '
                      << doc.ownedObj.toString();
            return;
        }

        fixUpInfo.toRefetch.insert(doc);
    }

    StatusWith<FixUpInfo> syncRollbackFindCommonPoint(OperationContext* txn,
                                                      DBClientConnection* them) {
        OldClientContext ctx(txn, rsOplogName);
        FixUpInfo fixUpInfo;

        boost::scoped_ptr<PlanExecutor> exec(
                InternalPlanner::collectionScan(txn,
                                                rsOplogName,
                                                ctx.db()->getCollection(rsOplogName),
                                                InternalPlanner::BACKWARD));

        BSONObj ourObj;
        RecordId ourLoc;

        if (PlanExecutor::ADVANCED != exec->getNext(&ourObj, &ourLoc)) {
            return StatusWith<FixUpInfo>(ErrorCodes::OplogStartMissing, "no oplog during initsync");
        }

        const Query query = Query().sort(reverseNaturalObj);
        const BSONObj fields = BSON("ts" << 1 << "h" << 1);

        //auto_ptr<DBClientCursor> u = us->query(rsOplogName, query, 0, 0, &fields, 0, 0);

        fixUpInfo.rbid = getRBID(them);
        auto_ptr<DBClientCursor> oplogCursor = them->query(rsOplogName, query, 0, 0, &fields, 0, 0);

        if (oplogCursor.get() == NULL || !oplogCursor->more())
            throw RSFatalException("remote oplog empty or unreadable");

        Timestamp ourTime = ourObj["ts"].timestamp();
        BSONObj theirObj = oplogCursor->nextSafe();
        Timestamp theirTime = theirObj["ts"].timestamp();

        long long diff = static_cast<long long>(ourTime.getSecs())
                               - static_cast<long long>(theirTime.getSecs());
        // diff could be positive, negative, or zero
        log() << "rollback our last optime:   " << ourTime.toStringPretty();
        log() << "rollback their last optime: " << theirTime.toStringPretty();
        log() << "rollback diff in end of log times: " << diff << " seconds";
        if (diff > 1800) {
            severe() << "rollback too long a time period for a rollback.";
            throw RSFatalException("rollback error: not willing to roll back "
                                   "more than 30 minutes of data");
        }

        unsigned long long scanned = 0;
        while (1) {
            scanned++;
            // todo add code to assure no excessive scanning for too long
            if (ourTime == theirTime) {
                if (ourObj["h"].Long() == theirObj["h"].Long()) {
                    // found the point back in time where we match.
                    // todo : check a few more just to be careful about hash collisions.
                    log() << "rollback found matching events at "
                          << ourTime.toStringPretty();
                    log() << "rollback findcommonpoint scanned : " << scanned;
                    fixUpInfo.commonPoint = ourTime;
                    fixUpInfo.commonPointOurDiskloc = ourLoc;
                    break;
                }

                refetch(fixUpInfo, ourObj);

                if (!oplogCursor->more()) {
                    severe() << "rollback error RS100 reached beginning of remote oplog";
                    log() << "  them:      " << them->toString() << " scanned: " << scanned;
                    log() << "  theirTime: " << theirTime.toStringLong();
                    log() << "  ourTime:   " << ourTime.toStringLong();
                    throw RSFatalException("RS100 reached beginning of remote oplog [2]");
                }
                theirObj = oplogCursor->nextSafe();
                theirTime = theirObj["ts"].timestamp();

                if (PlanExecutor::ADVANCED != exec->getNext(&ourObj, &ourLoc)) {
                    severe() << "rollback error RS101 reached beginning of local oplog";
                    log() << "  them:      " << them->toString() << " scanned: " << scanned;
                    log() << "  theirTime: " << theirTime.toStringLong();
                    log() << "  ourTime:   " << ourTime.toStringLong();
                    throw RSFatalException("RS101 reached beginning of local oplog [1]");
                }
                ourTime = ourObj["ts"].timestamp();
            }
            else if (theirTime > ourTime) {
                if (!oplogCursor->more()) {
                    severe() << "rollback error RS100 reached beginning of remote oplog";
                    log() << "  them:      " << them->toString() << " scanned: "
                          << scanned;
                    log() << "  theirTime: " << theirTime.toStringLong();
                    log() << "  ourTime:   " << ourTime.toStringLong();
                    throw RSFatalException("RS100 reached beginning of remote oplog [1]");
                }
                theirObj = oplogCursor->nextSafe();
                theirTime = theirObj["ts"].timestamp();
            }
            else {
                // theirTime < ourTime
                refetch(fixUpInfo, ourObj);
                if (PlanExecutor::ADVANCED != exec->getNext(&ourObj, &ourLoc)) {
                    severe() << "rollback error RS101 reached beginning of local oplog";
                    log() << "  them:      " << them->toString() << " scanned: " << scanned;
                    log() << "  theirTime: " << theirTime.toStringLong();
                    log() << "  ourTime:   " << ourTime.toStringLong();
                    throw RSFatalException("RS101 reached beginning of local oplog [2]");
                }
                ourTime = ourObj["ts"].timestamp();
            }
        }

        return StatusWith<FixUpInfo>(fixUpInfo);
    }

    bool copyCollectionFromRemote(OperationContext* txn,
                                  const string& host,
                                  const string& ns,
                                  string& errmsg) {
        Cloner cloner;

        DBClientConnection *tmpConn = new DBClientConnection();
        // cloner owns _conn in auto_ptr
        cloner.setConnection(tmpConn);
        uassert(15908, errmsg,
                tmpConn->connect(HostAndPort(host), errmsg) && replAuthenticate(tmpConn));

        return cloner.copyCollection(txn, ns, BSONObj(), errmsg, true, false, true);
    }

    void syncFixUp(OperationContext* txn,
                   FixUpInfo& fixUpInfo,
                   OplogReader* oplogreader,
                   ReplicationCoordinator* replCoord) {
        DBClientConnection* them = oplogreader->conn();

        // fetch all first so we needn't handle interruption in a fancy way

        unsigned long long totalSize = 0;

        list< pair<DocID, BSONObj> > goodVersions;

        BSONObj newMinValid;

        // fetch all the goodVersions of each document from current primary
        DocID doc;
        unsigned long long numFetched = 0;
        try {
            for (set<DocID>::iterator it = fixUpInfo.toRefetch.begin();
                    it != fixUpInfo.toRefetch.end();
                    it++) {
                doc = *it;

                verify(!doc._id.eoo());

                {
                    // TODO : slow.  lots of round trips.
                    numFetched++;
                    BSONObj good = them->findOne(doc.ns, doc._id.wrap(),
                                                     NULL, QueryOption_SlaveOk).getOwned();
                    totalSize += good.objsize();
                    uassert(13410, "replSet too much data to roll back",
                            totalSize < 300 * 1024 * 1024);

                    // note good might be eoo, indicating we should delete it
                    goodVersions.push_back(pair<DocID, BSONObj>(doc,good));
                }
            }
            newMinValid = oplogreader->getLastOp(rsOplogName);
            if (newMinValid.isEmpty()) {
                error() << "rollback error newMinValid empty?";
                return;
            }
        }
        catch (DBException& e) {
            LOG(1) << "rollback re-get objects: " << e.toString();
            error() << "rollback couldn't re-get ns:" << doc.ns << " _id:" << doc._id << ' '
                    << numFetched << '/' << fixUpInfo.toRefetch.size();
            throw e;
        }

        log() << "rollback 3.5";
        if (fixUpInfo.rbid != getRBID(oplogreader->conn())) {
            // our source rolled back itself.  so the data we received isn't necessarily consistent.
            warning() << "rollback rbid on source changed during rollback, cancelling this attempt";
            return;
        }

        // update them
        log() << "rollback 4 n:" << goodVersions.size();

        bool warn = false;

        invariant(!fixUpInfo.commonPointOurDiskloc.isNull());
        invariant(txn->lockState()->isW());

        // we have items we are writing that aren't from a point-in-time.  thus best not to come
        // online until we get to that point in freshness.
        Timestamp minValid = newMinValid["ts"].timestamp();
        log() << "minvalid=" << minValid.toStringLong();
        setMinValid(txn, minValid);

        // any full collection resyncs required?
        if (!fixUpInfo.collectionsToResyncData.empty()
                || !fixUpInfo.collectionsToResyncMetadata.empty()) {

            for (const string& ns : fixUpInfo.collectionsToResyncData) {
                log() << "rollback 4.1.1 coll resync " << ns;

                fixUpInfo.collectionsToResyncMetadata.erase(ns);

                const NamespaceString nss(ns);

                Database* db = dbHolder().openDb(txn, nss.db().toString());
                invariant(db);

                {
                    WriteUnitOfWork wunit(txn);
                    db->dropCollection(txn, ns);
                    wunit.commit();
                }

                {
                    string errmsg;

                    // This comes as a GlobalWrite lock, so there is no DB to be acquired after
                    // resume, so we can skip the DB stability checks. Also 
                    // copyCollectionFromRemote will acquire its own database pointer, under the
                    // appropriate locks, so just releasing and acquiring the lock is safe.
                    invariant(txn->lockState()->isW());
                    Lock::TempRelease release(txn->lockState());

                    bool ok = copyCollectionFromRemote(txn, them->getServerAddress(), ns, errmsg);
                    uassert(15909, str::stream() << "replSet rollback error resyncing collection "
                                                 << ns << ' ' << errmsg, ok);
                }
            }

            for (const string& ns : fixUpInfo.collectionsToResyncMetadata) {
                log() << "rollback 4.1.2 coll metadata resync " << ns;

                const NamespaceString nss(ns);
                auto db = dbHolder().openDb(txn, nss.db().toString());
                invariant(db);
                auto collection = db->getCollection(ns);
                invariant(collection);
                auto cce = collection->getCatalogEntry();

                const std::list<BSONObj> info =
                    them->getCollectionInfos(nss.db().toString(), BSON("name" << nss.coll()));

                if (info.empty()) {
                    // Collection dropped by "them" so we should drop it too.
                    log() << ns << " not found on remote host, dropping";
                    fixUpInfo.toDrop.insert(ns);
                    continue;
                }

                invariant(info.size() == 1);

                CollectionOptions options;
                auto status = options.parse(info.front());
                if (!status.isOK()) {
                    throw RSFatalException(str::stream() << "Failed to parse options "
                                                         << info.front() << ": "
                                                         << status.toString());
                }

                WriteUnitOfWork wuow(txn);
                if (options.flagsSet || cce->getCollectionOptions(txn).flagsSet) {
                    cce->updateFlags(txn, options.flags);
                }

                status = collection->setValidator(txn, options.validator);
                if (!status.isOK()) {
                    throw RSFatalException(str::stream() << "Failed to set validator: "
                                                         << status.toString());
                }
                wuow.commit();
            }

            // we did more reading from primary, so check it again for a rollback (which would mess
            // us up), and make minValid newer.
            log() << "rollback 4.2";

            string err;
            try {
                newMinValid = oplogreader->getLastOp(rsOplogName);
                if (newMinValid.isEmpty()) {
                    err = "can't get minvalid from sync source";
                }
                else {
                    Timestamp minValid = newMinValid["ts"].timestamp();
                    log() << "minvalid=" << minValid.toStringLong();
                    setMinValid(txn, minValid);
                }
            }
            catch (DBException& e) {
                err = "can't get/set minvalid: ";
                err += e.what();
            }
            if (fixUpInfo.rbid != getRBID(oplogreader->conn())) {
                // our source rolled back itself.  so the data we received isn't necessarily
                // consistent. however, we've now done writes.  thus we have a problem.
                err += "rbid at primary changed during resync/rollback";
            }
            if (!err.empty()) {
                severe() << "rolling back : " << err
                        << ". A full resync will be necessary.";
                // TODO: reset minvalid so that we are permanently in fatal state
                // TODO: don't be fatal, but rather, get all the data first.
                throw RSFatalException();
            }
            log() << "rollback 4.3";
        }

        map<string,shared_ptr<Helpers::RemoveSaver> > removeSavers;

        log() << "rollback 4.6";
        // drop collections to drop before doing individual fixups - that might make things faster
        // below actually if there were subsequent inserts to rollback
        for (set<string>::iterator it = fixUpInfo.toDrop.begin();
                it != fixUpInfo.toDrop.end();
                it++) {
            log() << "rollback drop: " << *it;

            Database* db = dbHolder().get(txn, nsToDatabaseSubstring(*it));
            if (db) {
                WriteUnitOfWork wunit(txn);

                shared_ptr<Helpers::RemoveSaver>& removeSaver = removeSavers[*it];
                if (!removeSaver)
                    removeSaver.reset(new Helpers::RemoveSaver("rollback", "", *it));

                // perform a collection scan and write all documents in the collection to disk
                boost::scoped_ptr<PlanExecutor> exec(
                        InternalPlanner::collectionScan(txn,
                                                        *it,
                                                        db->getCollection(*it)));
                BSONObj curObj;
                PlanExecutor::ExecState execState;
                while (PlanExecutor::ADVANCED == (execState = exec->getNext(&curObj, NULL))) {
                    removeSaver->goingToDelete(curObj);
                }
                if (execState != PlanExecutor::IS_EOF) {
                    if (execState == PlanExecutor::FAILURE &&
                            WorkingSetCommon::isValidStatusMemberObject(curObj)) {
                        Status errorStatus = WorkingSetCommon::getMemberObjectStatus(curObj);
                        severe() << "rolling back createCollection on " << *it
                                 << " failed with " << errorStatus
                                 << ". A full resync is necessary.";
                    }
                    else {
                        severe() << "rolling back createCollection on " << *it
                                 << " failed. A full resync is necessary.";
                    }
                            
                    throw RSFatalException();
                }

                db->dropCollection(txn, *it);
                wunit.commit();
            }
        }

        log() << "rollback 4.7";
        OldClientContext ctx(txn, rsOplogName);
        Collection* oplogCollection = ctx.db()->getCollection(rsOplogName);
        uassert(13423,
                str::stream() << "replSet error in rollback can't find " << rsOplogName,
                oplogCollection);

        unsigned deletes = 0, updates = 0;
        time_t lastProgressUpdate = time(0);
        time_t progressUpdateGap = 10;
        for (list<pair<DocID, BSONObj> >::iterator it = goodVersions.begin();
                it != goodVersions.end();
                it++) {
            time_t now = time(0);
            if (now - lastProgressUpdate > progressUpdateGap) {
                log() << deletes << " delete and "
                      << updates << " update operations processed out of "
                      << goodVersions.size() << " total operations";
                lastProgressUpdate = now;
            }
            const DocID& doc = it->first;
            BSONObj pattern = doc._id.wrap(); // { _id : ... }
            try {
                verify(doc.ns && *doc.ns);
                if (fixUpInfo.collectionsToResyncData.count(doc.ns)) {
                    // we just synced this entire collection
                    continue;
                }

                // keep an archive of items rolled back
                shared_ptr<Helpers::RemoveSaver>& removeSaver = removeSavers[doc.ns];
                if (!removeSaver)
                    removeSaver.reset(new Helpers::RemoveSaver("rollback", "", doc.ns));

                // todo: lots of overhead in context, this can be faster
                OldClientContext ctx(txn, doc.ns);

                // Add the doc to our rollback file
                BSONObj obj;
                Collection* collection = ctx.db()->getCollection(doc.ns);

                // Do not log an error when undoing an insert on a no longer existent collection.
                // It is likely that the collection was dropped as part of rolling back a
                // createCollection command and regardless, the document no longer exists.
                if (collection) {
                    bool found = Helpers::findOne(txn, collection, pattern, obj, false);
                    if (found) {
                        removeSaver->goingToDelete(obj);
                    }
                    else {
                        error() << "rollback cannot find object: " << pattern
                                << " in namespace " << doc.ns;
                    }
                }

                if (it->second.isEmpty()) {
                    // wasn't on the primary; delete.
                    // TODO 1.6 : can't delete from a capped collection.  need to handle that here.
                    deletes++;

                    if (collection) {
                        if (collection->isCapped()) {
                            // can't delete from a capped collection - so we truncate instead. if
                            // this item must go, so must all successors!!!
                            try {
                                // TODO: IIRC cappedTruncateAfter does not handle completely empty.
                                // this will crazy slow if no _id index.
                                long long start = Listener::getElapsedTimeMillis();
                                RecordId loc = Helpers::findOne(txn, collection, pattern, false);
                                if (Listener::getElapsedTimeMillis() - start > 200)
                                    warning() << "roll back slow no _id index for "
                                          << doc.ns << " perhaps?";
                                // would be faster but requires index:
                                // RecordId loc = Helpers::findById(nsd, pattern);
                                if (!loc.isNull()) {
                                    try {
                                        collection->temp_cappedTruncateAfter(txn, loc, true);
                                    }
                                    catch (DBException& e) {
                                        if (e.getCode() == 13415) {
                                            // hack: need to just make cappedTruncate do this...
                                            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                                                WriteUnitOfWork wunit(txn);
                                                uassertStatusOK(collection->truncate(txn));
                                                wunit.commit();
                                            } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
                                                                            txn,
                                                                            "truncate",
                                                                            collection->ns().ns());
                                        }
                                        else {
                                            throw e;
                                        }
                                    }
                                }
                            }
                            catch (DBException& e) {
                                error() << "rolling back capped collection rec "
                                      << doc.ns << ' ' << e.toString();
                            }
                        }
                        else {
                            deleteObjects(txn, 
                                          ctx.db(),
                                          doc.ns,
                                          pattern,
                                          PlanExecutor::YIELD_MANUAL,
                                          true,     // justone
                                          true);    // god
                        }
                        // did we just empty the collection?  if so let's check if it even
                        // exists on the source.
                        if (collection->numRecords(txn) == 0) {
                            try {
                                std::list<BSONObj> lst =
                                    them->getCollectionInfos( ctx.db()->name(),
                                                              BSON( "name" << nsToCollectionSubstring( doc.ns ) ) );
                                if (lst.empty()) {
                                    // we should drop
                                    WriteUnitOfWork wunit(txn);
                                    ctx.db()->dropCollection(txn, doc.ns);
                                    wunit.commit();
                                }
                            }
                            catch (DBException&) {
                                // this isn't *that* big a deal, but is bad.
                                warning() << "rollback error querying for existence of "
                                      << doc.ns << " at the primary, ignoring";
                            }
                        }
                    }
                }
                else {
                    // TODO faster...
                    OpDebug debug;
                    updates++;

                    const NamespaceString requestNs(doc.ns);
                    UpdateRequest request(requestNs);

                    request.setQuery(pattern);
                    request.setUpdates(it->second);
                    request.setGod();
                    request.setUpsert();
                    UpdateLifecycleImpl updateLifecycle(true, requestNs);
                    request.setLifecycle(&updateLifecycle);

                    update(txn, ctx.db(), request, &debug);

                }
            }
            catch (DBException& e) {
                log() << "exception in rollback ns:" << doc.ns << ' ' << pattern.toString()
                      << ' ' << e.toString() << " ndeletes:" << deletes;
                warn = true;
            }
        }

        removeSavers.clear(); // this effectively closes all of them
        log() << "rollback 5 d:" << deletes << " u:" << updates;
        log() << "rollback 6";

        // clean up oplog
        LOG(2) << "rollback truncate oplog after " <<
                fixUpInfo.commonPoint.toStringPretty();
        // TODO: fatal error if this throws?
        oplogCollection->temp_cappedTruncateAfter(txn, fixUpInfo.commonPointOurDiskloc, false);

        Status status = getGlobalAuthorizationManager()->initialize(txn);
        if (!status.isOK()) {
            warning() << "Failed to reinitialize auth data after rollback: " << status;
            warn = true;
        }

        // Reload the lastOpTimeApplied value in the replcoord and the lastAppliedHash value in 
        // bgsync to reflect our new last op.
        replCoord->resetLastOpTimeFromOplog(txn);
        BackgroundSync::get()->loadLastAppliedHash(txn);

        // done
        if (warn)
            warning() << "issues during syncRollback, see log";
        else
            log() << "rollback done";
    }

    unsigned _syncRollback(OperationContext* txn,
                           OplogReader* oplogreader,
                           ReplicationCoordinator* replCoord) {
        invariant(!txn->lockState()->isLocked());

        log() << "rollback 0";

        Lock::GlobalWrite globalWrite(txn->lockState(), 20000);
        if (!globalWrite.isLocked()) {
            warning() << "rollback couldn't get write lock in a reasonable time";
            return 2;
        }

        /** by doing this, we will not service reads (return an error as we aren't in secondary
         *  state. that perhaps is moot because of the write lock above, but that write lock
         *  probably gets deferred or removed or yielded later anyway.
         *
         *  also, this is better for status reporting - we know what is happening.
         */
        if (!replCoord->setFollowerMode(MemberState::RS_ROLLBACK)) {
            warning() << "Cannot transition from " << replCoord->getMemberState() <<
                " to " << MemberState(MemberState::RS_ROLLBACK);
            return 0;
        }

        FixUpInfo how;
        log() << "rollback 1";
        {
            oplogreader->resetCursor();

            log() << "rollback 2 FindCommonPoint";
            try {
                StatusWith<FixUpInfo> res = syncRollbackFindCommonPoint(txn, oplogreader->conn());
                if (!res.isOK()) {
                    switch (res.getStatus().code()) {
                        case ErrorCodes::OplogStartMissing:
                            return 1;
                        default:
                            throw new RSFatalException(res.getStatus().toString());
                    }
                }
                else {
                    how  = res.getValue();
                }
            }
            catch (RSFatalException& e) {
                error() << string(e.what());
                fassertFailedNoTrace(18752);
                return 2;
            }
            catch (DBException& e) {
                warning() << "rollback 2 exception " << e.toString() << "; sleeping 1 min";

                // Release the GlobalWrite lock while sleeping. We should always come here with a
                // GlobalWrite lock
                invariant(txn->lockState()->isW());
                Lock::TempRelease(txn->lockState());

                sleepsecs(60);
                throw;
            }
        }

        log() << "rollback 3 fixup";

        replCoord->incrementRollbackID();
        try {
            syncFixUp(txn, how, oplogreader, replCoord);
        }
        catch (RSFatalException& e) {
            error() << "exception during rollback: " << e.what();
            fassertFailedNoTrace(18753);
            return 2;
        }
        catch (...) {
            replCoord->incrementRollbackID();

            if (!replCoord->setFollowerMode(MemberState::RS_RECOVERING)) {
                warning() << "Failed to transition into " <<
                    MemberState(MemberState::RS_RECOVERING) << "; expected to be in state " <<
                    MemberState(MemberState::RS_ROLLBACK) << "but found self in " <<
                    replCoord->getMemberState();
            }

            throw;
        }
        replCoord->incrementRollbackID();

        // success - leave "ROLLBACK" state
        // can go to SECONDARY once minvalid is achieved
        if (!replCoord->setFollowerMode(MemberState::RS_RECOVERING)) {
            warning() << "Failed to transition into " << MemberState(MemberState::RS_RECOVERING) <<
                "; expected to be in state " << MemberState(MemberState::RS_ROLLBACK) <<
                "but found self in " << replCoord->getMemberState();
        }

        return 0;
    }
} // namespace

    void syncRollback(OperationContext* txn,
                      Timestamp lastOpTimeApplied,
                      OplogReader* oplogreader, 
                      ReplicationCoordinator* replCoord) {
        // check that we are at minvalid, otherwise we cannot rollback as we may be in an
        // inconsistent state
        {
            Timestamp minvalid = getMinValid(txn);
            if( minvalid > lastOpTimeApplied ) {
                severe() << "need to rollback, but in inconsistent state" << endl;
                log() << "minvalid: " << minvalid.toString() << " our last optime: "
                      << lastOpTimeApplied.toString() << endl;
                fassertFailedNoTrace(18750);
                return;
            }
        }

        log() << "beginning rollback" << rsLog;

        DisableDocumentValidation validationDisabler(txn);
        txn->setReplicatedWrites(false);
        unsigned s = _syncRollback(txn, oplogreader, replCoord);
        if (s)
            sleepsecs(s);
        
        log() << "rollback finished" << rsLog;
    }

} // namespace repl
} // namespace mongo
