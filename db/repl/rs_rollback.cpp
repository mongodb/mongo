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
*/

#include "pch.h"
#include "../client.h"
#include "../../client/dbclient.h"
#include "rs.h"
#include "../repl.h"
#include "../query.h"

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

        OpTime commonPoint;
        DiskLoc commonPointOurDiskloc;
    };

    static void syncRollbackFindCommonPoint(DBClientConnection *them, HowToFixUp& h) { 
        static time_t last;
        if( time(0)-last < 60 ) { 
            // this could put a lot of load on someone else, don't repeat too often
            sleepsecs(10);
            throw "findcommonpoint waiting a while before trying again";
        }
        last = time(0);

        assert( dbMutex.atLeastReadLocked() );
        NamespaceDetails *nsd = nsdetails(rsoplog);
        assert(nsd);
        Client::Context c(rsoplog, dbpath, 0, false);
        ReverseCappedCursor u(nsd);
        if( !u.ok() )
            throw "our oplog empty or unreadable";

        const Query q = Query().sort(reverseNaturalObj);
        const bo fields = BSON( "ts" << 1 << "h" << 1 );

        //auto_ptr<DBClientCursor> u = us->query(rsoplog, q, 0, 0, &fields, 0, 0);
        auto_ptr<DBClientCursor> t = them->query(rsoplog, q, 0, 0, &fields, 0, 0);

        if( !t->more() ) throw "remote oplog empty or unreadable";

        BSONObj ourObj = u.current();
        OpTime ourTime = ourObj["ts"]._opTime();
        BSONObj theirObj = t->nextSafe();
        OpTime theirTime = theirObj["ts"]._opTime();

        if( 1 ) {
            long long diff = (long long) ourTime.getSecs() - ((long long) theirTime.getSecs());
            /* diff could be positive, negative, or zero */
            log() << "replSet info syncRollback diff in end of log times : " << diff << " seconds" << rsLog;
            if( diff > 3600 ) { 
                log() << "replSet syncRollback too long a time period for a rollback." << rsLog;
                throw "error not willing to roll back more than one hour of data";
            }
        }

        unsigned long long totSize = 0;
        unsigned long long scanned = 0;
        while( 1 ) {
            scanned++;
            /* todo add code to assure no excessive scanning for too long */
            if( ourTime == theirTime ) { 
                if( ourObj["h"].Long() == theirObj["h"].Long() ) { 
                    // found the point back in time where we match.
                    // todo : check a few more just to be careful about hash collisions.
                    log() << "replSet rollback found matching events at " << ourTime.toStringPretty() << rsLog;
                    log() << "replSet scanned : " << scanned << rsLog;
                    h.commonPoint = ourTime;
                    h.commonPointOurDiskloc = u.currLoc();
                    return;
                }
                theirObj = t->nextSafe();
                theirTime = theirObj["ts"]._opTime();
                u.advance();
                if( !u.ok() ) throw "reached beginning of local oplog";
                ourObj = u.current();
                ourTime = ourObj["ts"]._opTime();
            }
            else if( theirTime > ourTime ) { 
                /* todo: we could hit beginning of log here.  exception thrown is ok but not descriptive, so fix up */
                theirObj = t->nextSafe();
                theirTime = theirObj["ts"]._opTime();
            }
            else { 
                // theirTime < ourTime
                totSize += ourObj.objsize();
                if( totSize > 512 * 1024 * 1024 )
                    throw "rollback too large";
                const char *op = ourObj.getStringField("op");
                if( *op != 'n' ) { // n == no-op
                    DocID d;
                    d.ns = ourObj.getStringField("ns");
                    if( *d.ns == 0 ) { 
                        log() << "replSet WARNING ignoring op on rollback TODO : " << ourObj.toString() << rsLog;
                    }
                    else {
                        d._id = ourObj["_id"];
                        if( d._id.eoo() ) {
                            log() << "replSet WARNING ignoring op on rollback no _id TODO : " << ourObj.toString() << rsLog;
                        }
                        else { 
                            h.toRefetch.insert(d);
                        }
                    }
                }
                u.advance();
                if( !u.ok() ) throw "reached beginning of local oplog";
                ourObj = u.current();
                ourTime = ourObj["ts"]._opTime();
            }
        }
    }

    struct X { 
        const bson::bo *op;
        bson::bo goodVersionOfObject;
    };

   void ReplSetImpl::syncFixUp(HowToFixUp& h, DBClientConnection *them) {
       // fetch all first so we needn't handle interruption in a fancy way

       unsigned long long totSize = 0;

       list< pair<DocID,bo> > goodVersions;

       for( set<DocID>::iterator i = h.toRefetch.begin(); i != h.toRefetch.end(); i++ ) { 
           const DocID& d = *i;

           assert( !d._id.eoo() );

           {
               /* TODO : slow.  lots of round trips. */
               bo good= them->findOne(d.ns, d._id.wrap()).getOwned();
               totSize += good.objsize();
               uassert( 13410, "replSet too much data to roll back", totSize < 300 * 1024 * 1024 );

               // note good might be eoo, indicating we should delete it
               goodVersions.push_back(pair<DocID,bo>(d,good));
           }
       }

       // update them
       sethbmsg(str::stream() << "syncRollback 4 n:" << goodVersions.size());

       bool warn = false;

       assert( !h.commonPointOurDiskloc.isNull() );

       MemoryMappedFile::flushAll(true);

       dbMutex.assertWriteLocked();

       NamespaceDetails *oplogDetails = nsdetails(rsoplog);
       uassert(13412, str::stream() << "replSet error in rollback can't find " << rsoplog, oplogDetails);

       for( list<pair<DocID,bo> >::iterator i = goodVersions.begin(); i != goodVersions.end(); i++ ) {
           const DocID& d = i->first;
           bo pattern = d._id.wrap(); // { _id : ... }
           try { 
               assert( d.ns && *d.ns );
               // todo: lots of overhead in context, this can be faster
               Client::Context c(d.ns, dbpath, 0, /*doauth*/false);
               if( i->second.isEmpty() ) {
                   // wasn't on the primary; delete.
                   /* TODO1.6 : can't delete from a capped collection.  need to handle that here. */
                   try { 
                       deleteObjects(d.ns, pattern, /*justone*/true, /*logop*/false, /*god*/true);
                   }
                   catch(...) { 
                       log() << "replSet rollback delete failed - todo finish capped collection support ns:" << d.ns << rsLog;
                   }
                }
               else {
                   // todo faster...
                   OpDebug debug;
                   _updateObjects(/*god*/true, d.ns, i->second, pattern, /*upsert=*/true, /*multi=*/false , /*logtheop=*/false , debug);
               }
           }
           catch(DBException& e) { 
               log() << "replSet exception in rollback ns:" << d.ns << ' ' << pattern.toString() << ' ' << e.toString() << rsLog;
               warn = true;
           }
       }

       // clean up oplog
       oplogDetails->cappedTruncateAfter(rsoplog, h.commonPointOurDiskloc, false);

       MemoryMappedFile::flushAll(true);

       // done
       if( warn ) 
           sethbmsg("issues during syncRollback, see log");
       else
           sethbmsg("syncRollback done");
   }

    void ReplSetImpl::syncRollback(OplogReader&r) { 
        assert( !lockedByMe() );
        assert( !dbMutex.atLeastReadLocked() );

        sethbmsg("syncRollback 0");

        writelocktry lk(rsoplog, 20000);
        if( !lk.got() ) {
            sethbmsg("syncRollback couldn't get write lock in a reasonable time");
            sleepsecs(2);
            return;
        }

        HowToFixUp how;
        sethbmsg("syncRollback 1");
        {
            r.resetCursor();
            /*DBClientConnection us(false, 0, 0);
            string errmsg;
            if( !us.connect(HostAndPort::me().toString(),errmsg) ) { 
                sethbmsg("syncRollback connect to self failure" + errmsg);
                return;
            }*/

            sethbmsg("syncRollback 2 FindCommonPoint");
            try {
                syncRollbackFindCommonPoint(r.conn(), how);
            }
            catch( const char *p ) { 
                sethbmsg(string("syncRollback 2 error ") + p);
                sleepsecs(10);
                return;
            }
            catch( DBException& e ) { 
                sethbmsg(string("syncRollback 2 exception ") + e.toString() + "; sleeping 1 min");
                sleepsecs(60);
                throw;
            }
        }

        sethbmsg("replSet syncRollback 3");

        syncFixUp(how, r.conn());
    }

}
