/* @file rs_rollback.cpp
*
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/pch.h"

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/client.h"
#include "mongo/db/cloner.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/operation_context_impl.h"

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
namespace replset {

    using namespace bson;

    void incRBID();

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

        set<string> collectionsToResync;

        OpTime commonPoint;
        DiskLoc commonPointOurDiskloc;

        int rbid; // remote server's current rollback sequence #
    };

    static void refetch(FixUpInfo& fixUpInfo, const BSONObj& ourObj) {
        const char* op = ourObj.getStringField("op");
        if (*op == 'n')
            return;

        if (ourObj.objsize() > 512 * 1024 * 1024)
            throw RSFatalException("rollback too large");

        DocID doc;
        // NOTE The assigned ns value may become invalid if we yield.
        doc.ns = ourObj.getStringField("ns");
        if (*doc.ns == '\0') {
            log() << "replSet WARNING ignoring op on rollback no ns TODO : "
                  << ourObj.toString() << rsLog;
            return;
        }

        BSONObj obj = ourObj.getObjectField(*op=='u' ? "o2" : "o");
        if (obj.isEmpty()) {
            log() << "replSet warning ignoring op on rollback : " << ourObj.toString() << rsLog;
            return;
        }

        if (*op == 'c') {
            BSONElement first = obj.firstElement();
            NamespaceString nss(doc.ns); // foo.$cmd
            string cmdname = first.fieldName();
            Command* cmd = Command::findCommand(cmdname.c_str());
            if (cmd == NULL) {
                log() << "replSet warning rollback no suchcommand " << first.fieldName()
                      << " - different mongod versions perhaps?" << rsLog;
                return;
            }
            else {
                if (cmdname == "create") {
                    // Create collection operation
                    // { ts: ..., h: ..., op: "c", ns: "foo.$cmd", o: { create: "abc", ... } }
                    string ns = nss.db().toString() + '.' + obj["create"].String(); // -> foo.abc
                    fixUpInfo.toDrop.insert(ns);
                    return;
                }
                else if (cmdname == "drop") {
                    string ns = nss.db().toString() + '.' + first.valuestr();
                    fixUpInfo.collectionsToResync.insert(ns);
                    return;
                }
                else if (cmdname == "dropIndexes" || cmdname == "deleteIndexes") {
                    // TODO: this is bad.  we simply full resync the collection here,
                    //       which could be very slow.
                    log() << "replSet info rollback of dropIndexes is slow in this version of "
                          << "mongod" << rsLog;
                    string ns = nss.db().toString() + '.' + first.valuestr();
                    fixUpInfo.collectionsToResync.insert(ns);
                    return;
                }
                else if (cmdname == "renameCollection") {
                    // TODO: slow.
                    log() << "replSet info rollback of renameCollection is slow in this version of "
                          << "mongod" << rsLog;
                    string from = first.valuestr();
                    string to = obj["to"].String();
                    fixUpInfo.collectionsToResync.insert(from);
                    fixUpInfo.collectionsToResync.insert(to);
                    return;
                }
                else if (cmdname == "reIndex") {
                    return;
                }
                else if (cmdname == "dropDatabase") {
                    log() << "replSet error rollback : can't rollback drop database full resync "
                          << "will be required" << rsLog;
                    log() << "replSet " << obj.toString() << rsLog;
                    throw RSFatalException();
                }
                else if (cmdname == "collMod") {
                    if (obj.nFields() == 2 && obj["usePowerOf2Sizes"].type() == Bool) {
                        log() << "replSet not rolling back change of usePowerOf2Sizes: " << obj;
                    }
                    else {
                        log() << "replSet error cannot rollback a collMod command: " << obj;
                        throw RSFatalException();
                    }
                }
                else {
                    log() << "replSet error can't rollback this command yet: "
                          << obj.toString() << rsLog;
                    log() << "replSet cmdname=" << cmdname << rsLog;
                    throw RSFatalException();
                }
            }
        }

        doc._id = obj["_id"];
        if (doc._id.eoo()) {
            log() << "replSet WARNING ignoring op on rollback no _id TODO : " << doc.ns << ' '
                  << ourObj.toString() << rsLog;
            return;
        }

        fixUpInfo.toRefetch.insert(doc);
    }

    int getRBID(DBClientConnection*);

    static void syncRollbackFindCommonPoint(DBClientConnection* them, FixUpInfo& fixUpInfo) {
        verify(Lock::isLocked());
        Client::Context ctx(rsoplog);

        boost::scoped_ptr<Runner> runner(
                InternalPlanner::collectionScan(rsoplog,
                                                ctx.db()->getCollection(rsoplog),
                                                InternalPlanner::BACKWARD));

        BSONObj ourObj;
        DiskLoc ourLoc;

        if (Runner::RUNNER_ADVANCED != runner->getNext(&ourObj, &ourLoc)) {
            throw RSFatalException("our oplog empty or unreadable");
        }

        const Query query = Query().sort(reverseNaturalObj);
        const BSONObj fields = BSON("ts" << 1 << "h" << 1);

        //auto_ptr<DBClientCursor> u = us->query(rsoplog, query, 0, 0, &fields, 0, 0);

        fixUpInfo.rbid = getRBID(them);
        auto_ptr<DBClientCursor> oplogCursor = them->query(rsoplog, query, 0, 0, &fields, 0, 0);

        if (oplogCursor.get() == NULL || !oplogCursor->more())
            throw RSFatalException("remote oplog empty or unreadable");

        OpTime ourTime = ourObj["ts"]._opTime();
        BSONObj theirObj = oplogCursor->nextSafe();
        OpTime theirTime = theirObj["ts"]._opTime();

        long long diff = static_cast<long long>(ourTime.getSecs())
                               - static_cast<long long>(theirTime.getSecs());
        // diff could be positive, negative, or zero
        log() << "replSet info rollback our last optime:   " << ourTime.toStringPretty() << rsLog;
        log() << "replSet info rollback their last optime: " << theirTime.toStringPretty() << rsLog;
        log() << "replSet info rollback diff in end of log times: " << diff << " seconds" << rsLog;
        if (diff > 1800) {
            log() << "replSet rollback too long a time period for a rollback." << rsLog;
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
                    log() << "replSet rollback found matching events at "
                          << ourTime.toStringPretty() << rsLog;
                    log() << "replSet rollback findcommonpoint scanned : " << scanned << rsLog;
                    fixUpInfo.commonPoint = ourTime;
                    fixUpInfo.commonPointOurDiskloc = ourLoc;
                    return;
                }

                refetch(fixUpInfo, ourObj);

                if (!oplogCursor->more()) {
                    log() << "replSet rollback error RS100 reached beginning of remote oplog"
                          << rsLog;
                    log() << "replSet   them:      " << them->toString() << " scanned: "
                          << scanned << rsLog;
                    log() << "replSet   theirTime: " << theirTime.toStringLong() << rsLog;
                    log() << "replSet   ourTime:   " << ourTime.toStringLong() << rsLog;
                    throw RSFatalException("RS100 reached beginning of remote oplog [2]");
                }
                theirObj = oplogCursor->nextSafe();
                theirTime = theirObj["ts"]._opTime();

                if (Runner::RUNNER_ADVANCED != runner->getNext(&ourObj, &ourLoc)) {
                    log() << "replSet rollback error RS101 reached beginning of local oplog"
                          << rsLog;
                    log() << "replSet   them:      " << them->toString() << " scanned: "
                          << scanned << rsLog;
                    log() << "replSet   theirTime: " << theirTime.toStringLong() << rsLog;
                    log() << "replSet   ourTime:   " << ourTime.toStringLong() << rsLog;
                    throw RSFatalException("RS101 reached beginning of local oplog [1]");
                }
                ourTime = ourObj["ts"]._opTime();
            }
            else if (theirTime > ourTime) {
                if (!oplogCursor->more()) {
                    log() << "replSet rollback error RS100 reached beginning of remote oplog"
                          << rsLog;
                    log() << "replSet   them:      " << them->toString() << " scanned: "
                          << scanned << rsLog;
                    log() << "replSet   theirTime: " << theirTime.toStringLong() << rsLog;
                    log() << "replSet   ourTime:   " << ourTime.toStringLong() << rsLog;
                    throw RSFatalException("RS100 reached beginning of remote oplog [1]");
                }
                theirObj = oplogCursor->nextSafe();
                theirTime = theirObj["ts"]._opTime();
            }
            else {
                // theirTime < ourTime
                refetch(fixUpInfo, ourObj);
                if (Runner::RUNNER_ADVANCED != runner->getNext(&ourObj, &ourLoc)) {
                    log() << "replSet rollback error RS101 reached beginning of local oplog"
                          << rsLog;
                    log() << "replSet   them:      " << them->toString() << " scanned: "
                          << scanned << rsLog;
                    log() << "replSet   theirTime: " << theirTime.toStringLong() << rsLog;
                    log() << "replSet   ourTime:   " << ourTime.toStringLong() << rsLog;
                    throw RSFatalException("RS101 reached beginning of local oplog [2]");
                }
                ourTime = ourObj["ts"]._opTime();
            }
        }
    }

    void ReplSetImpl::syncFixUp(FixUpInfo& fixUpInfo, OplogReader& oplogreader) {
        DBClientConnection* them = oplogreader.conn();
        OperationContextImpl txn;

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
            newMinValid = oplogreader.getLastOp(rsoplog);
            if (newMinValid.isEmpty()) {
                sethbmsg("rollback error newMinValid empty?");
                return;
            }
        }
        catch (DBException& e) {
            sethbmsg(str::stream() << "rollback re-get objects: " << e.toString(),0);
            log() << "rollback couldn't re-get ns:" << doc.ns << " _id:" << doc._id << ' '
                  << numFetched << '/' << fixUpInfo.toRefetch.size() << rsLog;
            throw e;
        }

        MemoryMappedFile::flushAll(true);

        sethbmsg("rollback 3.5");
        if (fixUpInfo.rbid != getRBID(oplogreader.conn())) {
            // our source rolled back itself.  so the data we received isn't necessarily consistent.
            sethbmsg("rollback rbid on source changed during rollback, cancelling this attempt");
            return;
        }

        // update them
        sethbmsg(str::stream() << "rollback 4 n:" << goodVersions.size());

        bool warn = false;

        verify(!fixUpInfo.commonPointOurDiskloc.isNull());
        verify(Lock::isW());

        // we have items we are writing that aren't from a point-in-time.  thus best not to come
        // online until we get to that point in freshness.
        log() << "replSet minvalid=" << newMinValid["ts"]._opTime().toStringLong() << rsLog;
        setMinValid(newMinValid);

        // any full collection resyncs required?
        if (!fixUpInfo.collectionsToResync.empty()) {
            for (set<string>::iterator it = fixUpInfo.collectionsToResync.begin();
                    it != fixUpInfo.collectionsToResync.end();
                    it++) {
                string ns = *it;
                sethbmsg(str::stream() << "rollback 4.1 coll resync " << ns);

                Client::Context ctx(ns);
                ctx.db()->dropCollection(&txn, ns);
                {
                    string errmsg;
                    dbtemprelease release;
                    bool ok = Cloner::copyCollectionFromRemote(&txn, them->getServerAddress(),
                                                               ns, errmsg);
                    uassert(15909, str::stream() << "replSet rollback error resyncing collection "
                                                 << ns << ' ' << errmsg, ok);
                }
            }

            // we did more reading from primary, so check it again for a rollback (which would mess
            // us up), and make minValid newer.
            sethbmsg("rollback 4.2");

            string err;
            try {
                newMinValid = oplogreader.getLastOp(rsoplog);
                if (newMinValid.isEmpty()) {
                    err = "can't get minvalid from primary";
                }
                else {
                    log() << "replSet minvalid=" << newMinValid["ts"]._opTime().toStringLong()
                          << rsLog;
                    setMinValid(newMinValid);
                }
            }
            catch (DBException& e) {
                err = "can't get/set minvalid: ";
                err += e.what();
            }
            if (fixUpInfo.rbid != getRBID(oplogreader.conn())) {
                // our source rolled back itself.  so the data we received isn't necessarily
                // consistent. however, we've now done writes.  thus we have a problem.
                err += "rbid at primary changed during resync/rollback";
            }
            if (!err.empty()) {
                log() << "replSet error rolling back : " << err
                      << ". A full resync will be necessary." << rsLog;
                // TODO: reset minvalid so that we are permanently in fatal state
                // TODO: don't be fatal, but rather, get all the data first.
                sethbmsg("rollback error");
                throw RSFatalException();
            }
            sethbmsg("rollback 4.3");
        }

        sethbmsg("rollback 4.6");
        // drop collections to drop before doing individual fixups - that might make things faster
        // below actually if there were subsequent inserts to rollback
        for (set<string>::iterator it = fixUpInfo.toDrop.begin();
                it != fixUpInfo.toDrop.end();
                it++) {
            Client::Context ctx(*it);
            log() << "replSet rollback drop: " << *it << rsLog;
            ctx.db()->dropCollection(&txn, *it);
        }

        sethbmsg("rollback 4.7");
        Client::Context ctx(rsoplog);
        Collection* oplogCollection = ctx.db()->getCollection(rsoplog);
        uassert(13423,
                str::stream() << "replSet error in rollback can't find " << rsoplog,
                oplogCollection);

        map<string,shared_ptr<Helpers::RemoveSaver> > removeSavers;

        unsigned deletes = 0, updates = 0;
        for (list<pair<DocID, BSONObj> >::iterator it = goodVersions.begin();
                it != goodVersions.end();
                it++) {
            const DocID& doc = it->first;
            BSONObj pattern = doc._id.wrap(); // { _id : ... }
            try {
                verify(doc.ns && *doc.ns);
                if (fixUpInfo.collectionsToResync.count(doc.ns)) {
                    // we just synced this entire collection
                    continue;
                }

                txn.recoveryUnit()->commitIfNeeded();

                // keep an archive of items rolled back
                shared_ptr<Helpers::RemoveSaver>& removeSaver = removeSavers[doc.ns];
                if (!removeSaver)
                    removeSaver.reset(new Helpers::RemoveSaver("rollback", "", doc.ns));

                // todo: lots of overhead in context, this can be faster
                Client::Context ctx(doc.ns);

                // Add the doc to our rollback file
                BSONObj obj;
                bool found = Helpers::findOne(ctx.db()->getCollection(doc.ns), pattern, obj, false);
                if (found) {
                    removeSaver->goingToDelete(obj);
                }
                else {
                    error() << "rollback cannot find object by id";
                }

                if (it->second.isEmpty()) {
                    // wasn't on the primary; delete.
                    // TODO 1.6 : can't delete from a capped collection.  need to handle that here.
                    deletes++;

                    Collection* collection = ctx.db()->getCollection(doc.ns);
                    if (collection) {
                        if (collection->isCapped()) {
                            // can't delete from a capped collection - so we truncate instead. if
                            // this item must go, so must all successors!!!
                            try {
                                // TODO: IIRC cappedTruncateAfter does not handle completely empty.
                                // this will crazy slow if no _id index.
                                long long start = Listener::getElapsedTimeMillis();
                                DiskLoc loc = Helpers::findOne(collection, pattern, false);
                                if (Listener::getElapsedTimeMillis() - start > 200)
                                    log() << "replSet warning roll back slow no _id index for "
                                          << doc.ns << " perhaps?" << rsLog;
                                // would be faster but requires index:
                                // DiskLoc loc = Helpers::findById(nsd, pattern);
                                if (!loc.isNull()) {
                                    try {
                                        collection->temp_cappedTruncateAfter(&txn, loc, true);
                                    }
                                    catch (DBException& e) {
                                        if (e.getCode() == 13415) {
                                            // hack: need to just make cappedTruncate do this...
                                            uassertStatusOK(collection->truncate(&txn));
                                        }
                                        else {
                                            throw e;
                                        }
                                    }
                                }
                            }
                            catch (DBException& e) {
                                log() << "replSet error rolling back capped collection rec "
                                      << doc.ns << ' ' << e.toString() << rsLog;
                            }
                        }
                        else {
                            deletes++;
                            deleteObjects(&txn, 
                                          ctx.db(),
                                          doc.ns,
                                          pattern,
                                          true,     // justone
                                          false,    // logop
                                          true);    // god
                        }
                        // did we just empty the collection?  if so let's check if it even
                        // exists on the source.
                        if (collection->numRecords() == 0) {
                            try {
                                string sys = ctx.db()->name() + ".system.namespaces";
                                BSONObj nsResult = them->findOne(sys, QUERY("name" << doc.ns));
                                if (nsResult.isEmpty()) {
                                    // we should drop
                                    ctx.db()->dropCollection(&txn, doc.ns);
                                }
                            }
                            catch (DBException&) {
                                // this isn't *that* big a deal, but is bad.
                                log() << "replSet warning rollback error querying for existence of "
                                      << doc.ns << " at the primary, ignoring" << rsLog;
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

                    update(&txn, ctx.db(), request, &debug);

                }
            }
            catch (DBException& e) {
                log() << "replSet exception in rollback ns:" << doc.ns << ' ' << pattern.toString()
                      << ' ' << e.toString() << " ndeletes:" << deletes << rsLog;
                warn = true;
            }
        }

        removeSavers.clear(); // this effectively closes all of them

        sethbmsg(str::stream() << "rollback 5 d:" << deletes << " u:" << updates);
        MemoryMappedFile::flushAll(true);
        sethbmsg("rollback 6");

        // clean up oplog
        LOG(2) << "replSet rollback truncate oplog after " << fixUpInfo.commonPoint.toStringPretty()
               << rsLog;
        // TODO: fatal error if this throws?
        oplogCollection->temp_cappedTruncateAfter(&txn, fixUpInfo.commonPointOurDiskloc, false);

        Status status = getGlobalAuthorizationManager()->initialize();
        if (!status.isOK()) {
            warning() << "Failed to reinitialize auth data after rollback: " << status;
            warn = true;
        }

        // reset cached lastoptimewritten and h value
        loadLastOpTimeWritten();

        sethbmsg("rollback 7");
        MemoryMappedFile::flushAll(true);

        // done
        if (warn)
            sethbmsg("issues during syncRollback, see log");
        else
            sethbmsg("rollback done");
    }

    void ReplSetImpl::syncRollback(OplogReader& oplogreader) {
        // check that we are at minvalid, otherwise we cannot rollback as we may be in an
        // inconsistent state
        {
            Lock::DBRead lk("local.replset.minvalid");
            BSONObj mv;
            if (Helpers::getSingleton("local.replset.minvalid", mv)) {
                OpTime minvalid = mv["ts"]._opTime();
                if (minvalid > lastOpTimeWritten) {
                    log() << "replSet need to rollback, but in inconsistent state";
                    log() << "minvalid: " << minvalid.toString() << " our last optime: "
                          << lastOpTimeWritten.toString();
                    changeState(MemberState::RS_FATAL);
                    return;
                }
            }
        }

        unsigned s = _syncRollback(oplogreader);
        if (s)
            sleepsecs(s);
    }

    unsigned ReplSetImpl::_syncRollback(OplogReader& oplogreader) {
        verify(!lockedByMe());
        verify(!Lock::isLocked());

        sethbmsg("rollback 0");

        writelocktry lk(20000);
        if (!lk.got()) {
            sethbmsg("rollback couldn't get write lock in a reasonable time");
            return 2;
        }

        if (state().secondary()) {
            /** by doing this, we will not service reads (return an error as we aren't in secondary
             *  state. that perhaps is moot because of the write lock above, but that write lock
             *  probably gets deferred or removed or yielded later anyway.
             *
             *  also, this is better for status reporting - we know what is happening.
             */
            changeState(MemberState::RS_ROLLBACK);
        }

        FixUpInfo how;
        sethbmsg("rollback 1");
        {
            oplogreader.resetCursor();

            sethbmsg("rollback 2 FindCommonPoint");
            try {
                syncRollbackFindCommonPoint(oplogreader.conn(), how);
            }
            catch (RSFatalException& e) {
                sethbmsg(string(e.what()));
                _fatal();
                return 2;
            }
            catch (DBException& e) {
                sethbmsg(string("rollback 2 exception ") + e.toString() + "; sleeping 1 min");
                dbtemprelease release;
                sleepsecs(60);
                throw;
            }
        }

        sethbmsg("replSet rollback 3 fixup");

        incRBID();
        try {
            syncFixUp(how, oplogreader);
        }
        catch (RSFatalException& e) {
            sethbmsg("rollback fixup error");
            log() << "exception during rollback: " << e.what();
            _fatal();
            return 2;
        }
        catch (...) {
            incRBID();
            throw;
        }
        incRBID();

        // success - leave "ROLLBACK" state
        // can go to SECONDARY once minvalid is achieved
        changeState(MemberState::RS_RECOVERING);

        return 0;
    }

} // namespace replset
} // namespace mongo
