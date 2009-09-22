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
     local.oplog.$main is the default
*/

#pragma once

#include "pdfile.h"
#include "db.h"
#include "dbhelpers.h"
#include "query.h"

#include "../client/dbclient.h"

#include "../util/optime.h"

namespace mongo {

    class DBClientConnection;
    class DBClientCursor;

	/* replication slave? (possibly with slave or repl pair nonmaster)
       --slave cmd line setting -> SimpleSlave
	*/
	typedef enum { NotSlave=0, SimpleSlave, ReplPairSlave } SlaveTypes;
	extern SlaveTypes slave;

	/* true means we are master and doing replication.  if we are not writing to oplog (no --master or repl pairing), 
	   this won't be true.
	*/
    extern bool master;

    extern int opIdMem;
    
    bool cloneFrom(const char *masterHost, string& errmsg, const string& fromdb, bool logForReplication, 
				   bool slaveOk, bool useReplAuth, bool snapshot);

    /* A replication exception */
    class SyncException : public DBException {
    public:
        virtual const char* what() const throw() { return "sync exception"; }
    };
    
    /* A Source is a source from which we can pull (replicate) data.
       stored in collection local.sources.

       Can be a group of things to replicate for several databases.

          { host: ..., source: ..., only: ..., syncedTo: ..., localLogTs: ..., dbsNextPass: { ... }, incompleteCloneDbs: { ... } }

       'source' defaults to 'main'; support for multiple source names is
       not done (always use main for now).
    */
    class ReplSource {
        bool resync(string db);
        bool sync_pullOpLog(int& nApplied);
        void sync_pullOpLog_applyOperation(BSONObj& op, OpTime *localLogTail);
        
        auto_ptr<DBClientConnection> conn;
        auto_ptr<DBClientCursor> cursor;

        set<string> addDbNextPass;
        set<string> incompleteCloneDbs;

        ReplSource();
        
        // returns the dummy ns used to do the drop
        string resyncDrop( const char *db, const char *requester );
        // returns true if connected on return
        bool connect();
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
        
    public:
        static void applyOperation(const BSONObj& op);
        bool replacing; // in "replace mode" -- see CmdReplacePeer
        bool paired; // --pair in use
        string hostName;    // ip addr or hostname plus optionally, ":<port>"
        string _sourceName;  // a logical source name.
        string sourceName() const {
            return _sourceName.empty() ? "main" : _sourceName;
        }
        string only; // only a certain db. note that in the sources collection, this may not be changed once you start replicating.

        /* the last time point we have already synced up to. */
        OpTime syncedTo;
        OpTime lastSavedLocalTs_;

        int nClonedThisPass;

        typedef vector< shared_ptr< ReplSource > > SourceVector;
        static void loadAll(SourceVector&);
        explicit ReplSource(BSONObj);
        bool sync(int& nApplied);
        void save(); // write ourself to local.sources
        void resetConnection() {
            cursor = auto_ptr<DBClientCursor>(0);
            conn = auto_ptr<DBClientConnection>(0);
        }

        // make a jsobj from our member fields of the form
        //   { host: ..., source: ..., syncedTo: ... }
        BSONObj jsobj();

        bool operator==(const ReplSource&r) const {
            return hostName == r.hostName && sourceName() == r.sourceName();
        }
        operator string() const { return sourceName() + "@" + hostName; }
        
        bool haveMoreDbsToSync() const { return !addDbNextPass.empty(); }        

        static bool throttledForceResyncDead( const char *requester );
        static void forceResyncDead( const char *requester );
        void forceResync( const char *requester );
    };

    /* Write operation to the log (local.oplog.$main)
       "i" insert
       "u" update
       "d" delete
       "c" db cmd
       "db" declares presence of a database (ns is set to the db name + '.')
    */
    void _logOp(const char *opstr, const char *ns, const char *logNs, const BSONObj& obj, BSONObj *patt, bool *b, const OpTime &ts);
    void logOp(const char *opstr, const char *ns, const BSONObj& obj, BSONObj *patt = 0, bool *b = 0);

    // class for managing a set of ids in memory
    class MemIds {
    public:
        MemIds() : size_() {}
        friend class IdTracker;
        void reset() { imp_.clear(); }
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
    class IdTracker {
    public:
        IdTracker() :
        dbIds_( "local.temp.replIds" ),
        dbModIds_( "local.temp.replModIds" ),
        inMem_( true ),
        maxMem_( opIdMem ) {
        }
        void reset( int maxMem = opIdMem ) {
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
    
} // namespace mongo
