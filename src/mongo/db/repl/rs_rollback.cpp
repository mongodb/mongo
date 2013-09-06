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

#include "mongo/db/client.h"
#include "mongo/db/cloner.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/rs.h"

/* Scenarios

   We went offline with ops not replicated out.

       F = node that failed and coming back.
       P = node that took over, new primary

   #1:
       F : a b c d e f g
       P : a b c d q

   The design is "keep P".  One could argue here that "keep F" has some merits, however, in most cases P
   will have significantly more data.  Also note that P may have a proper subset of F's stream if there were
   no subsequent writes.

   For now the model is simply : get F back in sync with P.  If P was really behind or something, we should have
   just chosen not to fail over anyway.

   #2:
       F : a b c d e f g                -> a b c d
       P : a b c d

   #3:
       F : a b c d e f g                -> a b c d q r s t u v w x z
       P : a b c d.q r s t u v w x z

   Steps
    find an event in common. 'd'.
    undo our events beyond that by:
      (1) taking copy from other server of those objects
      (2) do not consider copy valid until we pass reach an optime after when we fetched the new version of object
          -- i.e., reset minvalid.
      (3) we could skip operations on objects that are previous in time to our capture of the object as an optimization.

*/

namespace mongo {

    using namespace bson;

    void incRBID();

    class rsfatal : public std::exception {
    public:
        rsfatal(std::string m = "replica set fatal exception") : msg(m) {}
        virtual ~rsfatal() throw() {};
        virtual const char* what() const throw() { return msg.c_str(); }
    private:
        std::string msg;
    };

    struct DocID {
        const char *ns;
        be _id;
        bool operator<(const DocID& d) const {
            int c = strcmp(ns, d.ns);
            if( c < 0 ) return true;
            if( c > 0 ) return false;
            return _id < d._id;
        }
    };

    struct HowToFixUp {
        /* note this is a set -- if there are many $inc's on a single document we need to rollback, we only
           need to refetch it once. */
        set<DocID> toRefetch;

        /* collections to drop */
        set<string> toDrop;

        set<string> collectionsToResync;

        OpTime commonPoint;
        DiskLoc commonPointOurDiskloc;

        int rbid; // remote server's current rollback sequence #
    };

    static void refetch(HowToFixUp& h, const BSONObj& ourObj) {
        const char *op = ourObj.getStringField("op");
        if( *op == 'n' )
            return;

        unsigned long long totSize = 0;
        totSize += ourObj.objsize();
        if( totSize > 512 * 1024 * 1024 )
            throw "rollback too large";

        DocID d;
        // NOTE The assigned ns value may become invalid if we yield.
        d.ns = ourObj.getStringField("ns");
        if( *d.ns == 0 ) {
            log() << "replSet WARNING ignoring op on rollback no ns TODO : " << ourObj.toString() << rsLog;
            return;
        }

        bo o = ourObj.getObjectField(*op=='u' ? "o2" : "o");
        if( o.isEmpty() ) {
            log() << "replSet warning ignoring op on rollback : " << ourObj.toString() << rsLog;
            return;
        }

        if( *op == 'c' ) {
            be first = o.firstElement();
            NamespaceString s(d.ns); // foo.$cmd
            string cmdname = first.fieldName();
            Command *cmd = Command::findCommand(cmdname.c_str());
            if( cmd == 0 ) {
                log() << "replSet warning rollback no suchcommand " << first.fieldName() << " - different mongod versions perhaps?" << rsLog;
                return;
            }
            else {
                /* findandmodify - tranlated?
                   godinsert?,
                   renamecollection a->b.  just resync a & b
                */
                if( cmdname == "create" ) {
                    /* Create collection operation
                       { ts: ..., h: ..., op: "c", ns: "foo.$cmd", o: { create: "abc", ... } }
                    */
                    string ns = s.db().toString() + '.' + o["create"].String(); // -> foo.abc
                    h.toDrop.insert(ns);
                    return;
                }
                else if( cmdname == "drop" ) {
                    string ns = s.db().toString() + '.' + first.valuestr();
                    h.collectionsToResync.insert(ns);
                    return;
                }
                else if( cmdname == "dropIndexes" || cmdname == "deleteIndexes" ) {
                    /* TODO: this is bad.  we simply full resync the collection here, which could be very slow. */
                    log() << "replSet info rollback of dropIndexes is slow in this version of mongod" << rsLog;
                    string ns = s.db().toString() + '.' + first.valuestr();
                    h.collectionsToResync.insert(ns);
                    return;
                }
                else if( cmdname == "renameCollection" ) {
                    /* TODO: slow. */
                    log() << "replSet info rollback of renameCollection is slow in this version of mongod" << rsLog;
                    string from = first.valuestr();
                    string to = o["to"].String();
                    h.collectionsToResync.insert(from);
                    h.collectionsToResync.insert(to);
                    return;
                }
                else if( cmdname == "reIndex" ) {
                    return;
                }
                else if( cmdname == "dropDatabase" ) {
                    log() << "replSet error rollback : can't rollback drop database full resync will be required" << rsLog;
                    log() << "replSet " << o.toString() << rsLog;
                    throw rsfatal();
                }
                else {
                    log() << "replSet error can't rollback this command yet: " << o.toString() << rsLog;
                    log() << "replSet cmdname=" << cmdname << rsLog;
                    throw rsfatal();
                }
            }
        }

        d._id = o["_id"];
        if( d._id.eoo() ) {
            log() << "replSet WARNING ignoring op on rollback no _id TODO : " << d.ns << ' '<< ourObj.toString() << rsLog;
            return;
        }

        h.toRefetch.insert(d);
    }

    int getRBID(DBClientConnection*);

    static void syncRollbackFindCommonPoint(DBClientConnection *them, HowToFixUp& h) {
        verify( Lock::isLocked() );
        Client::Context c(rsoplog);
        NamespaceDetails *nsd = nsdetails(rsoplog);
        verify(nsd);
        ReverseCappedCursor u(nsd);
        if( !u.ok() )
            throw rsfatal("our oplog empty or unreadable");

        const Query q = Query().sort(reverseNaturalObj);
        const bo fields = BSON( "ts" << 1 << "h" << 1 );

        //auto_ptr<DBClientCursor> u = us->query(rsoplog, q, 0, 0, &fields, 0, 0);

        h.rbid = getRBID(them);
        auto_ptr<DBClientCursor> t = them->query(rsoplog, q, 0, 0, &fields, 0, 0);

        if( t.get() == 0 || !t->more() ) throw rsfatal("remote oplog empty or unreadable");

        BSONObj ourObj = u.current();
        OpTime ourTime = ourObj["ts"]._opTime();
        BSONObj theirObj = t->nextSafe();
        OpTime theirTime = theirObj["ts"]._opTime();

        {
            long long diff = (long long) ourTime.getSecs() - ((long long) theirTime.getSecs());
            /* diff could be positive, negative, or zero */
            log() << "replSet info rollback our last optime:   " << ourTime.toStringPretty() << rsLog;
            log() << "replSet info rollback their last optime: " << theirTime.toStringPretty() << rsLog;
            log() << "replSet info rollback diff in end of log times: " << diff << " seconds" << rsLog;
            if( diff > 1800 ) {
                log() << "replSet rollback too long a time period for a rollback." << rsLog;
                throw rsfatal(str::stream() << "rollback error: not willing to roll back "
                                            << "more than 30 minutes of data");
            }
        }

        unsigned long long scanned = 0;
        while( 1 ) {
            scanned++;
            /* todo add code to assure no excessive scanning for too long */
            if( ourTime == theirTime ) {
                if( ourObj["h"].Long() == theirObj["h"].Long() ) {
                    // found the point back in time where we match.
                    // todo : check a few more just to be careful about hash collisions.
                    log() << "replSet rollback found matching events at " << ourTime.toStringPretty() << rsLog;
                    log() << "replSet rollback findcommonpoint scanned : " << scanned << rsLog;
                    h.commonPoint = ourTime;
                    h.commonPointOurDiskloc = u.currLoc();
                    return;
                }

                refetch(h, ourObj);

                if( !t->more() ) {
                    log() << "replSet rollback error RS100 reached beginning of remote oplog" << rsLog;
                    log() << "replSet   them:      " << them->toString() << " scanned: " << scanned << rsLog;
                    log() << "replSet   theirTime: " << theirTime.toStringLong() << rsLog;
                    log() << "replSet   ourTime:   " << ourTime.toStringLong() << rsLog;
                    throw rsfatal("RS100 reached beginning of remote oplog [2]");
                }
                theirObj = t->nextSafe();
                theirTime = theirObj["ts"]._opTime();

                u.advance();
                if( !u.ok() ) {
                    log() << "replSet rollback error RS101 reached beginning of local oplog" << rsLog;
                    log() << "replSet   them:      " << them->toString() << " scanned: " << scanned << rsLog;
                    log() << "replSet   theirTime: " << theirTime.toStringLong() << rsLog;
                    log() << "replSet   ourTime:   " << ourTime.toStringLong() << rsLog;
                    throw rsfatal("RS101 reached beginning of local oplog [1]");
                }
                ourObj = u.current();
                ourTime = ourObj["ts"]._opTime();
            }
            else if( theirTime > ourTime ) {
                if( !t->more() ) {
                    log() << "replSet rollback error RS100 reached beginning of remote oplog" << rsLog;
                    log() << "replSet   them:      " << them->toString() << " scanned: " << scanned << rsLog;
                    log() << "replSet   theirTime: " << theirTime.toStringLong() << rsLog;
                    log() << "replSet   ourTime:   " << ourTime.toStringLong() << rsLog;
                    throw rsfatal("RS100 reached beginning of remote oplog [1]");
                }
                theirObj = t->nextSafe();
                theirTime = theirObj["ts"]._opTime();
            }
            else {
                // theirTime < ourTime
                refetch(h, ourObj);
                u.advance();
                if( !u.ok() ) {
                    log() << "replSet rollback error RS101 reached beginning of local oplog" << rsLog;
                    log() << "replSet   them:      " << them->toString() << " scanned: " << scanned << rsLog;
                    log() << "replSet   theirTime: " << theirTime.toStringLong() << rsLog;
                    log() << "replSet   ourTime:   " << ourTime.toStringLong() << rsLog;
                    throw rsfatal("RS101 reached beginning of local oplog [2]");
                }
                ourObj = u.current();
                ourTime = ourObj["ts"]._opTime();
            }
        }
    }

    struct X {
        const bson::bo *op;
        bson::bo goodVersionOfObject;
    };

    void ReplSetImpl::syncFixUp(HowToFixUp& h, OplogReader& r) {
        DBClientConnection *them = r.conn();

        // fetch all first so we needn't handle interruption in a fancy way

        unsigned long long totSize = 0;

        list< pair<DocID,bo> > goodVersions;

        bo newMinValid;

        /* fetch all the goodVersions of each document from current primary */
        DocID d;
        unsigned long long n = 0;
        try {
            for( set<DocID>::iterator i = h.toRefetch.begin(); i != h.toRefetch.end(); i++ ) {
                d = *i;

                verify( !d._id.eoo() );

                {
                    /* TODO : slow.  lots of round trips. */
                    n++;
                    bo good= them->findOne(d.ns, d._id.wrap(), NULL, QueryOption_SlaveOk).getOwned();
                    totSize += good.objsize();
                    uassert( 13410, "replSet too much data to roll back", totSize < 300 * 1024 * 1024 );

                    // note good might be eoo, indicating we should delete it
                    goodVersions.push_back(pair<DocID,bo>(d,good));
                }
            }
            newMinValid = r.getLastOp(rsoplog);
            if( newMinValid.isEmpty() ) {
                sethbmsg("rollback error newMinValid empty?");
                return;
            }
        }
        catch(DBException& e) {
            sethbmsg(str::stream() << "rollback re-get objects: " << e.toString(),0);
            log() << "rollback couldn't re-get ns:" << d.ns << " _id:" << d._id << ' ' << n << '/' << h.toRefetch.size() << rsLog;
            throw e;
        }

        MemoryMappedFile::flushAll(true);

        sethbmsg("rollback 3.5");
        if( h.rbid != getRBID(r.conn()) ) {
            // our source rolled back itself.  so the data we received isn't necessarily consistent.
            sethbmsg("rollback rbid on source changed during rollback, cancelling this attempt");
            return;
        }

        // update them
        sethbmsg(str::stream() << "rollback 4 n:" << goodVersions.size());

        bool warn = false;

        verify( !h.commonPointOurDiskloc.isNull() );
        verify( Lock::isW() );

        /* we have items we are writing that aren't from a point-in-time.  thus best not to come online
           until we get to that point in freshness. */
        log() << "replSet minvalid=" << newMinValid["ts"]._opTime().toStringLong() << rsLog;
        setMinValid(newMinValid);

        /** any full collection resyncs required? */
        if( !h.collectionsToResync.empty() ) {
            for( set<string>::iterator i = h.collectionsToResync.begin(); i != h.collectionsToResync.end(); i++ ) {
                string ns = *i;
                sethbmsg(str::stream() << "rollback 4.1 coll resync " << ns);

                Client::Context c(ns);
                {
                    bob res;
                    string errmsg;
                    dropCollection(ns, errmsg, res);
                    {
                        dbtemprelease r;
                        bool ok = Cloner::copyCollectionFromRemote(them->getServerAddress(), ns, errmsg);
                        uassert(15909, str::stream() << "replSet rollback error resyncing collection " << ns << ' ' << errmsg, ok);
                    }
                }
            }

            /* we did more reading from primary, so check it again for a rollback (which would mess us up), and
               make minValid newer.
               */
            sethbmsg("rollback 4.2");
            {
                string err;
                try {
                    newMinValid = r.getLastOp(rsoplog);
                    if( newMinValid.isEmpty() ) {
                        err = "can't get minvalid from primary";
                    }
                    else {
                        log() << "replSet minvalid=" << newMinValid["ts"]._opTime().toStringLong() << rsLog;
                        setMinValid(newMinValid);
                    }
                }
                catch (DBException&) {
                    err = "can't get/set minvalid";
                }
                if( h.rbid != getRBID(r.conn()) ) {
                    // our source rolled back itself.  so the data we received isn't necessarily consistent.
                    // however, we've now done writes.  thus we have a problem.
                    err += "rbid at primary changed during resync/rollback";
                }
                if( !err.empty() ) {
                    log() << "replSet error rolling back : " << err << ". A full resync will be necessary." << rsLog;
                    /* todo: reset minvalid so that we are permanently in fatal state */
                    /* todo: don't be fatal, but rather, get all the data first. */
                    sethbmsg("rollback error");
                    throw rsfatal();
                }
            }
            sethbmsg("rollback 4.3");
        }

        sethbmsg("rollback 4.6");
        /** drop collections to drop before doing individual fixups - that might make things faster below actually if there were subsequent inserts to rollback */
        for( set<string>::iterator i = h.toDrop.begin(); i != h.toDrop.end(); i++ ) {
            Client::Context c(*i);
            try {
                bob res;
                string errmsg;
                log() << "replSet rollback drop: " << *i << rsLog;
                dropCollection(*i, errmsg, res);
            }
            catch(...) {
                log() << "replset rollback error dropping collection " << *i << rsLog;
            }
        }

        sethbmsg("rollback 4.7");
        Client::Context c(rsoplog);
        NamespaceDetails *oplogDetails = nsdetails(rsoplog);
        uassert(13423, str::stream() << "replSet error in rollback can't find " << rsoplog, oplogDetails);

        map<string,shared_ptr<Helpers::RemoveSaver> > removeSavers;

        unsigned deletes = 0, updates = 0;
        for( list<pair<DocID,bo> >::iterator i = goodVersions.begin(); i != goodVersions.end(); i++ ) {
            const DocID& d = i->first;
            bo pattern = d._id.wrap(); // { _id : ... }
            try {
                verify( d.ns && *d.ns );
                if( h.collectionsToResync.count(d.ns) ) {
                    /* we just synced this entire collection */
                    continue;
                }

                getDur().commitIfNeeded();

                /* keep an archive of items rolled back */
                shared_ptr<Helpers::RemoveSaver>& rs = removeSavers[d.ns];
                if ( ! rs )
                    rs.reset( new Helpers::RemoveSaver( "rollback" , "" , d.ns ) );

                // todo: lots of overhead in context, this can be faster
                Client::Context c(d.ns);

                // Add the doc to our rollback file
                BSONObj obj;
                bool found = Helpers::findOne(d.ns, pattern, obj, false);
                if ( found ) {
                    rs->goingToDelete( obj );
                } else {
                    error() << "rollback cannot find object by id" << endl;
                }

                if( i->second.isEmpty() ) {
                    // wasn't on the primary; delete.
                    /* TODO1.6 : can't delete from a capped collection.  need to handle that here. */
                    deletes++;

                    NamespaceDetails *nsd = nsdetails(d.ns);
                    if( nsd ) {
                        if( nsd->isCapped() ) {
                            /* can't delete from a capped collection - so we truncate instead. if this item must go,
                            so must all successors!!! */
                            try {
                                /** todo: IIRC cappedTruncateAfter does not handle completely empty.  todo. */
                                // this will crazy slow if no _id index.
                                long long start = Listener::getElapsedTimeMillis();
                                DiskLoc loc = Helpers::findOne(d.ns, pattern, false);
                                if( Listener::getElapsedTimeMillis() - start > 200 )
                                    log() << "replSet warning roll back slow no _id index for " << d.ns << " perhaps?" << rsLog;
                                //would be faster but requires index: DiskLoc loc = Helpers::findById(nsd, pattern);
                                if( !loc.isNull() ) {
                                    try {
                                        nsd->cappedTruncateAfter(d.ns, loc, true);
                                    }
                                    catch(DBException& e) {
                                        if( e.getCode() == 13415 ) {
                                            // hack: need to just make cappedTruncate do this...
                                            nsd->emptyCappedCollection(d.ns);
                                        }
                                        else {
                                            throw;
                                        }
                                    }
                                }
                            }
                            catch(DBException& e) {
                                log() << "replSet error rolling back capped collection rec " << d.ns << ' ' << e.toString() << rsLog;
                            }
                        }
                        else {
                            try {
                                deletes++;
                                deleteObjects(d.ns, pattern, /*justone*/true, /*logop*/false, /*god*/true);
                            }
                            catch(...) {
                                log() << "replSet error rollback delete failed ns:" << d.ns << rsLog;
                            }
                        }
                        // did we just empty the collection?  if so let's check if it even exists on the source.
                        if( nsd->numRecords() == 0 ) {
                            try {
                                string sys = cc().database()->name() + ".system.namespaces";
                                bo o = them->findOne(sys, QUERY("name"<<d.ns));
                                if( o.isEmpty() ) {
                                    // we should drop
                                    try {
                                        bob res;
                                        string errmsg;
                                        dropCollection(d.ns, errmsg, res);
                                    }
                                    catch(...) {
                                        log() << "replset error rolling back collection " << d.ns << rsLog;
                                    }
                                }
                            }
                            catch(DBException& ) {
                                /* this isn't *that* big a deal, but is bad. */
                                log() << "replSet warning rollback error querying for existence of " << d.ns << " at the primary, ignoring" << rsLog;
                            }
                        }
                    }
                }
                else {
                    // todo faster...
                    OpDebug debug;
                    updates++;

                    const NamespaceString requestNs(d.ns);
                    UpdateRequest request(requestNs);

                    request.setQuery(pattern);
                    request.setUpdates(i->second);
                    request.setGod();
                    request.setUpsert();

                    update(request, &debug);

                }
            }
            catch(DBException& e) {
                log() << "replSet exception in rollback ns:" << d.ns << ' ' << pattern.toString() << ' ' << e.toString() << " ndeletes:" << deletes << rsLog;
                warn = true;
            }
        }

        removeSavers.clear(); // this effectively closes all of them

        sethbmsg(str::stream() << "rollback 5 d:" << deletes << " u:" << updates);
        MemoryMappedFile::flushAll(true);
        sethbmsg("rollback 6");

        // clean up oplog
        LOG(2) << "replSet rollback truncate oplog after " << h.commonPoint.toStringPretty() << rsLog;
        // todo: fatal error if this throws?
        oplogDetails->cappedTruncateAfter(rsoplog, h.commonPointOurDiskloc, false);

        /* reset cached lastoptimewritten and h value */
        loadLastOpTimeWritten();

        sethbmsg("rollback 7");
        MemoryMappedFile::flushAll(true);

        // done
        if( warn )
            sethbmsg("issues during syncRollback, see log");
        else
            sethbmsg("rollback done");
    }

    void ReplSetImpl::syncRollback(OplogReader&r) {
        // check that we are at minvalid, otherwise we cannot rollback as we may be in an
        // inconsistent state
        {
            Lock::DBRead lk("local.replset.minvalid");
            BSONObj mv;
            if( Helpers::getSingleton("local.replset.minvalid", mv) ) {
                OpTime minvalid = mv["ts"]._opTime();
                if( minvalid > lastOpTimeWritten ) {
                    log() << "replSet need to rollback, but in inconsistent state" << endl;
                    log() << "minvalid: " << minvalid.toString() << " our last optime: "
                          << lastOpTimeWritten.toString() << endl;
                    changeState(MemberState::RS_FATAL);
                    return;
                }
            }
        }

        unsigned s = _syncRollback(r);
        if( s )
            sleepsecs(s);
    }

    unsigned ReplSetImpl::_syncRollback(OplogReader&r) {
        verify( !lockedByMe() );
        verify( !Lock::isLocked() );

        sethbmsg("rollback 0");

        writelocktry lk(20000);
        if( !lk.got() ) {
            sethbmsg("rollback couldn't get write lock in a reasonable time");
            return 2;
        }

        if( state().secondary() ) {
            /* by doing this, we will not service reads (return an error as we aren't in secondary state.
               that perhaps is moot because of the write lock above, but that write lock probably gets deferred
               or removed or yielded later anyway.

               also, this is better for status reporting - we know what is happening.
               */
            changeState(MemberState::RS_ROLLBACK);
        }

        HowToFixUp how;
        sethbmsg("rollback 1");
        {
            r.resetCursor();

            sethbmsg("rollback 2 FindCommonPoint");
            try {
                syncRollbackFindCommonPoint(r.conn(), how);
            }
            catch( rsfatal& e ) {
                sethbmsg(string(e.what()));
                _fatal();
                return 2;
            }
            catch( DBException& e ) {
                sethbmsg(string("rollback 2 exception ") + e.toString() + "; sleeping 1 min");
                dbtemprelease r;
                sleepsecs(60);
                throw;
            }
        }

        sethbmsg("replSet rollback 3 fixup");

        {
            incRBID();
            try {
                syncFixUp(how, r);
            }
            catch( rsfatal& ) {
                sethbmsg("rollback fixup error");
                _fatal();
                return 2;
            }
            catch(...) {
                incRBID(); throw;
            }
            incRBID();

            /* success - leave "ROLLBACK" state
               can go to SECONDARY once minvalid is achieved
            */
            changeState(MemberState::RS_RECOVERING);
        }

        return 0;
    }

}
