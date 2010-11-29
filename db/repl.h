// repl.h - replication

/**
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

/* replication data overview

   at the slave:
     local.sources { host: ..., source: ..., only: ..., syncedTo: ..., localLogTs: ..., dbsNextPass: { ... }, incompleteCloneDbs: { ... } }

   at the master:
     local.oplog.$<source>
*/

#pragma once

#include "pdfile.h"
#include "db.h"
#include "dbhelpers.h"
#include "query.h"
#include "queryoptimizer.h"
#include "../client/dbclient.h"
#include "../util/optime.h"
#include "oplog.h"
#include "../util/concurrency/thread_pool.h"
#include "oplogreader.h"

namespace mongo {

	/* replication slave? (possibly with slave or repl pair nonmaster)
       --slave cmd line setting -> SimpleSlave
	*/
	typedef enum { NotSlave=0, SimpleSlave, ReplPairSlave } SlaveTypes;

    class ReplSettings {
    public:
        SlaveTypes slave;

        /* true means we are master and doing replication.  if we are not writing to oplog (no --master or repl pairing), 
           this won't be true.
        */
        bool master;

        int opIdMem;

        bool fastsync;
        
        bool autoresync;
        
        int slavedelay;

        ReplSettings()
            : slave(NotSlave) , master(false) , opIdMem(100000000) , fastsync() , autoresync(false), slavedelay() {
        }

    };

    extern ReplSettings replSettings;
    
    bool cloneFrom(const char *masterHost, string& errmsg, const string& fromdb, bool logForReplication, 
				   bool slaveOk, bool useReplAuth, bool snapshot);

    /* A replication exception */
    class SyncException : public DBException {
    public:
        SyncException() : DBException( "sync exception" , 10001 ){}
    };

    /* A Source is a source from which we can pull (replicate) data.
       stored in collection local.sources.

       Can be a group of things to replicate for several databases.

          { host: ..., source: ..., only: ..., syncedTo: ..., localLogTs: ..., dbsNextPass: { ... }, incompleteCloneDbs: { ... } }

       'source' defaults to 'main'; support for multiple source names is
       not done (always use main for now).
    */
    class ReplSource {
        auto_ptr<ThreadPool> tp;

        bool resync(string db);

        void sync_pullOpLog_applyOperation(BSONObj& op, OpTime *localLogTail, bool alreadyLocked);
        
        /* pull some operations from the master's oplog, and apply them. 
           calls sync_pullOpLog_applyOperation()
        */
        int sync_pullOpLog(int& nApplied);

        /* we only clone one database per pass, even if a lot need done.  This helps us
           avoid overflowing the master's transaction log by doing too much work before going
           back to read more transactions. (Imagine a scenario of slave startup where we try to
           clone 100 databases in one pass.)
        */
        set<string> addDbNextPass;

        set<string> incompleteCloneDbs;

        ReplSource();
        
        // returns the dummy ns used to do the drop
        string resyncDrop( const char *db, const char *requester );
        // returns possibly unowned id spec for the operation.
        static BSONObj idForOp( const BSONObj &op, bool &mod );
        static void updateSetsWithOp( const BSONObj &op, bool mayUpdateStorage );
        // call without the db mutex
        void syncToTailOfRemoteLog();
        // call with the db mutex
        OpTime nextLastSavedLocalTs() const;
        void setLastSavedLocalTs( const OpTime &nextLocalTs );
        // call without the db mutex
        void resetSlave();
        // call with the db mutex
        // returns false if the slave has been reset
        bool updateSetsWithLocalOps( OpTime &localLogTail, bool mayUnlock );
        string ns() const { return string( "local.oplog.$" ) + sourceName(); }
        unsigned _sleepAdviceTime;
        
    public:
        OplogReader oplogReader;

        static void applyOperation(const BSONObj& op);
        bool replacing; // in "replace mode" -- see CmdReplacePeer
        bool paired; // --pair in use
        string hostName;    // ip addr or hostname plus optionally, ":<port>"
        string _sourceName;  // a logical source name.
        string sourceName() const {
            return _sourceName.empty() ? "main" : _sourceName;
        }
        string only; // only a certain db. note that in the sources collection, this may not be changed once you start replicating.

        /* the last time point we have already synced up to (in the remote/master's oplog). */
        OpTime syncedTo;

        /* This is for repl pairs.
           _lastSavedLocalTs is the most recent point in the local log that we know is consistent
           with the remote log ( ie say the local op log has entries ABCDE and the remote op log 
           has ABCXY, then _lastSavedLocalTs won't be greater than C until we have reconciled 
           the DE-XY difference.)
        */
        OpTime _lastSavedLocalTs;

        int nClonedThisPass;

        typedef vector< shared_ptr< ReplSource > > SourceVector;
        static void loadAll(SourceVector&);
        explicit ReplSource(BSONObj);

        /* -1 = error */
        int sync(int& nApplied);

        void save(); // write ourself to local.sources

        // make a jsobj from our member fields of the form
        //   { host: ..., source: ..., syncedTo: ... }
        BSONObj jsobj();

        bool operator==(const ReplSource&r) const {
            return hostName == r.hostName && sourceName() == r.sourceName();
        }
        string toString() const { return sourceName() + "@" + hostName; }
        
        bool haveMoreDbsToSync() const { return !addDbNextPass.empty(); }        
        int sleepAdvice() const {
            if ( !_sleepAdviceTime )
                return 0;
            int wait = _sleepAdviceTime - unsigned( time( 0 ) );
            return wait > 0 ? wait : 0;
        }
        
        static bool throttledForceResyncDead( const char *requester );
        static void forceResyncDead( const char *requester );
        void forceResync( const char *requester );
    };

    // class for managing a set of ids in memory
    class MemIds {
    public:
        MemIds() : size_() {}
        friend class IdTracker;
        void reset() {
            imp_.clear();
            size_ = 0;
        }
        bool get( const char *ns, const BSONObj &id ) { return imp_[ ns ].count( id ); }
        void set( const char *ns, const BSONObj &id, bool val ) {
            if ( val ) {
                if ( imp_[ ns ].insert( id.getOwned() ).second ) {
                    size_ += id.objsize() + sizeof( BSONObj );
                }
            } else {
                if ( imp_[ ns ].erase( id ) == 1 ) {
                    size_ -= id.objsize() + sizeof( BSONObj );
                }
            }
        }
        long long roughSize() const {
            return size_;
        }
    private:
        typedef map< string, BSONObjSetDefaultOrder > IdSets;
        IdSets imp_;
        long long size_;
    };

    // class for managing a set of ids in a db collection
    // All functions must be called with db mutex held
    class DbIds {
    public:
        DbIds( const string & name ) : impl_( name, BSON( "ns" << 1 << "id" << 1 ) ) {}
        void reset() {
            impl_.reset();
        }
        bool get( const char *ns, const BSONObj &id ) {
            return impl_.get( key( ns, id ) );
        }
        void set( const char *ns, const BSONObj &id, bool val ) {
            impl_.set( key( ns, id ), val );
        }
    private:
        static BSONObj key( const char *ns, const BSONObj &id ) {
            BSONObjBuilder b;
            b << "ns" << ns;
            // rename _id to id since there may be duplicates
            b.appendAs( id.firstElement(), "id" );
            return b.obj();
        }        
        DbSet impl_;
    };

    // class for tracking ids and mod ids, in memory or on disk
    // All functions must be called with db mutex held
    // Kind of sloppy class structure, for now just want to keep the in mem
    // version speedy.
	// see http://www.mongodb.org/display/DOCS/Pairing+Internals
    class IdTracker {
    public:
        IdTracker() :
        dbIds_( "local.temp.replIds" ),
        dbModIds_( "local.temp.replModIds" ),
        inMem_( true ),
        maxMem_( replSettings.opIdMem ) {
        }
        void reset( int maxMem = replSettings.opIdMem ) {
            memIds_.reset();
            memModIds_.reset();
            dbIds_.reset();
            dbModIds_.reset();
            maxMem_ = maxMem;
            inMem_ = true;
        }
        bool haveId( const char *ns, const BSONObj &id ) {
            if ( inMem_ )
                return get( memIds_, ns, id );
            else
                return get( dbIds_, ns, id );
        }
        bool haveModId( const char *ns, const BSONObj &id ) {
            if ( inMem_ )
                return get( memModIds_, ns, id );
            else
                return get( dbModIds_, ns, id );
        }
        void haveId( const char *ns, const BSONObj &id, bool val ) {
            if ( inMem_ )
                set( memIds_, ns, id, val );
            else
                set( dbIds_, ns, id, val );
        }
        void haveModId( const char *ns, const BSONObj &id, bool val ) {
            if ( inMem_ )
                set( memModIds_, ns, id, val );
            else
                set( dbModIds_, ns, id, val );
        }
        // will release the db mutex
        void mayUpgradeStorage() {
            if ( !inMem_ || memIds_.roughSize() + memModIds_.roughSize() <= maxMem_ )
                return;
            log() << "saving master modified id information to collection" << endl;
            upgrade( memIds_, dbIds_ );
            upgrade( memModIds_, dbModIds_ );
            memIds_.reset();
            memModIds_.reset();
            inMem_ = false;
        }
        bool inMem() const { return inMem_; }
    private:
        template< class T >
        bool get( T &ids, const char *ns, const BSONObj &id ) {
            return ids.get( ns, id );
        }
        template< class T >
        void set( T &ids, const char *ns, const BSONObj &id, bool val ) {
            ids.set( ns, id, val );
        }
        void upgrade( MemIds &a, DbIds &b ) {
            for( MemIds::IdSets::const_iterator i = a.imp_.begin(); i != a.imp_.end(); ++i ) {
                for( BSONObjSetDefaultOrder::const_iterator j = i->second.begin(); j != i->second.end(); ++j ) {
                    set( b, i->first.c_str(), *j, true );            
                    RARELY {
                        dbtemprelease t;
                    }
                }
            }
        }
        MemIds memIds_;
        MemIds memModIds_;
        DbIds dbIds_;
        DbIds dbModIds_;
        bool inMem_;
        int maxMem_;
    };
    
    bool anyReplEnabled();
    void appendReplicationInfo( BSONObjBuilder& result , bool authed , int level = 0 );
    
    
} // namespace mongo
