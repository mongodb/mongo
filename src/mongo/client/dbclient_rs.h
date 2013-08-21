/** @file dbclient_rs.h Connect to a Replica Set, from C++ */

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "mongo/pch.h"

#include <boost/function.hpp>
#include <boost/shared_ptr.hpp>
#include <set>
#include <utility>

#include "mongo/client/dbclientinterface.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

    class ReplicaSetMonitor;
    class TagSet;
    struct ReadPreferenceSetting;
    typedef shared_ptr<ReplicaSetMonitor> ReplicaSetMonitorPtr;
    typedef pair<set<string>,set<int> > NodeDiff;

    /**
     * manages state about a replica set for client
     * keeps tabs on whose master and what slaves are up
     * can hand a slave to someone for SLAVE_OK
     * one instance per process per replica set
     * TODO: we might be able to use a regular Node * to avoid _lock
     */
    class ReplicaSetMonitor {
    public:
        typedef boost::function1<void,const ReplicaSetMonitor*> ConfigChangeHook;

        /**
         * Data structure for keeping track of the states of individual replica
         * members. This class is not thread-safe so proper measures should be taken
         * when sharing this object across multiple threads.
         *
         * Note: these get copied around in the nodes vector so be sure to maintain
         * copyable semantics here
         */
        struct Node {
            Node( const HostAndPort& a , DBClientConnection* c ) :
                addr( a ),
                conn(c),
                ok( c != NULL ),
                ismaster(false),
                secondary( false ),
                hidden( false ),
                pingTimeMillis( 0 ) {
            }

            bool okForSecondaryQueries() const {
                return ok && secondary && ! hidden;
            }

            /**
             * Checks if the given tag matches the tag attached to this node.
             *
             * Example:
             *
             * Tag of this node: { "dc": "nyc", "region": "na", "rack": "4" }
             *
             * match: {}
             * match: { "dc": "nyc", "rack": 4 }
             * match: { "region": "na", "dc": "nyc" }
             * not match: { "dc: "nyc", "rack": 2 }
             * not match: { "dc": "sf" }
             *
             * @param tag the tag to use to compare. Should not contain any
             *     embedded documents.
             *
             * @return true if the given tag matches the this node's tag
             *     specification
             */
            bool matchesTag(const BSONObj& tag) const;

            /**
             * @param  threshold  max ping time (in ms) to be considered local
             * @return true if node is a local secondary, and can handle queries
             **/
            bool isLocalSecondary( const int threshold ) const {
                return pingTimeMillis < threshold;
            }

            /**
             * Checks whether this nodes is compatible with the given readPreference and
             * tag. Compatibility check is strict in the sense that secondary preferred
             * is treated like secondary only and primary preferred is treated like
             * primary only.
             *
             * @return true if this node is compatible with the read preference and tags.
             */
            bool isCompatible(ReadPreference readPreference, const TagSet* tag) const;

            BSONObj toBSON() const;

            string toString() const {
                return toBSON().toString();
            }

            HostAndPort addr;
            boost::shared_ptr<DBClientConnection> conn;

            // if this node is in a failure state
            // used for slave routing
            // this is too simple, should make it better
            bool ok;

            // as reported by ismaster
            BSONObj lastIsMaster;

            bool ismaster;
            bool secondary;
            bool hidden;

            int pingTimeMillis;

        };

        static const double SOCKET_TIMEOUT_SECS;

        /**
         * Selects the right node given the nodes to pick from and the preference.
         *
         * @param nodes the nodes to select from
         * @param preference the read mode to use
         * @param tags the tags used for filtering nodes
         * @param localThresholdMillis the exclusive upper bound of ping time to be
         *     considered as a local node. Local nodes are favored over non-local
         *     nodes if multiple nodes matches the other criteria.
         * @param lastHost the host used in the last successful request. This is used for
         *     selecting a different node as much as possible, by doing a simple round
         *     robin, starting from the node next to this lastHost. This will be overwritten
         *     with the newly chosen host if not empty, not primary and when preference
         *     is not Nearest.
         * @param isPrimarySelected out parameter that is set to true if the returned host
         *     is a primary. Cannot be NULL and valid only if returned host is not empty.
         *
         * @return the host object of the node selected. If none of the nodes are
         *     eligible, returns an empty host.
         */
        static HostAndPort selectNode(const std::vector<Node>& nodes,
                                      ReadPreference preference,
                                      TagSet* tags,
                                      int localThresholdMillis,
                                      HostAndPort* lastHost,
                                      bool* isPrimarySelected);

        /**
         * Selects the right node given the nodes to pick from and the preference. This
         * will also attempt to refresh the local view of the replica set configuration
         * if the primary node needs to be returned but is not currently available (except
         * for ReadPrefrence_Nearest).
         *
         * @param preference the read mode to use.
         * @param tags the tags used for filtering nodes.
         * @param isPrimarySelected out parameter that is set to true if the returned host
         *     is a primary. Cannot be NULL and valid only if returned host is not empty.
         *
         * @return the host object of the node selected. If none of the nodes are
         *     eligible, returns an empty host.
         */
        HostAndPort selectAndCheckNode(ReadPreference preference,
                                       TagSet* tags,
                                       bool* isPrimarySelected);

        /**
         * Creates a new ReplicaSetMonitor, if it doesn't already exist.
         */
        static void createIfNeeded( const string& name , const vector<HostAndPort>& servers );

        /**
         * gets a cached Monitor per name. If the monitor is not found and createFromSeed is false,
         * it will return none. If createFromSeed is true, it will try to look up the last known
         * servers list for this set and will create a new monitor using that as the seed list.
         */
        static ReplicaSetMonitorPtr get( const string& name, const bool createFromSeed = false );

        /**
         * Populates activeSets with all the currently tracked replica set names.
         */
        static void getAllTrackedSets(set<string>* activeSets);

        /**
         * checks all sets for current master and new secondaries
         * usually only called from a BackgroundJob
         */
        static void checkAll();

        /**
         * Removes the ReplicaSetMonitor for the given set name from _sets, which will delete it.
         * If clearSeedCache is true, then the cached seed string for this Replica Set will be removed
         * from _seedServers.
         */
        static void remove( const string& name, bool clearSeedCache = false );

        static int getMaxFailedChecks() { return _maxFailedChecks; };
        static void setMaxFailedChecks(int numChecks) { _maxFailedChecks = numChecks; };

        /**
         * this is called whenever the config of any replica set changes
         * currently only 1 globally
         * asserts if one already exists
         * ownership passes to ReplicaSetMonitor and the hook will actually never be deleted
         */
        static void setConfigChangeHook( ConfigChangeHook hook );

        /**
         * Stops all monitoring on replica sets and clears all cached information as well.
         * Note that this does not prevent new monitors from being created afterwards or even
         * while this is being executed. As a consequence, NEVER call this if you have other
         * threads that has a DBClientReplicaSet instance or will create one before this
         * fully terminates as it will cause a deadlock. This is intended for performing cleanups
         * in unit tests.
         *
         * Warning: Make sure that the monitor thread is running, otherwise this can hang
         * indefinitely.
         */
        static void cleanup();

        ~ReplicaSetMonitor();

        /** @return HostAndPort or throws an exception */
        HostAndPort getMaster();

        /**
         * notify the monitor that server has faild
         */
        void notifyFailure( const HostAndPort& server );

        /**
         * @deprecated use #getCandidateNode instead
         * @return prev if its still ok, and if not returns a random slave that is ok for reads
         */
        HostAndPort getSlave( const HostAndPort& prev );

        /**
         * @param  preferLocal  Prefer a local secondary, otherwise pick any
         *                      secondary, or fall back to primary
         * @return a random slave that is ok for reads
         */
        HostAndPort getSlave( bool preferLocal = true );

        /**
         * notify the monitor that server has faild
         */
        void notifySlaveFailure( const HostAndPort& server );

        /**
         * checks for current master and new secondaries
         */
        void check();

        string getName() const { return _name; }

        string getServerAddress() const;

        bool contains( const string& server ) const;
        
        void appendInfo( BSONObjBuilder& b ) const;

        /**
         * Set the threshold value (in ms) for a node to be considered local.
         * NOTE:  This function acquires the _lock mutex.
         **/
        void setLocalThresholdMillis( const int millis );

        /**
         * @return true if the host is compatible with the given readPreference and tag set.
         */
        bool isHostCompatible(const HostAndPort& host, ReadPreference readPreference,
                const TagSet* tagSet) const;

        /**
         * Performs a quick check if at least one node is up based on the cached
         * view of the set.
         *
         * @return true if any node is ok
         */
        bool isAnyNodeOk() const;

    private:
        /**
         * This populates a list of hosts from the list of seeds (discarding the
         * seed list). Should only be called from within _setsLock.
         * @param name set name
         * @param servers seeds
         */
        ReplicaSetMonitor( const string& name , const vector<HostAndPort>& servers );

        static void _remove_inlock( const string& name, bool clearSeedCache = false );

        /**
         * Checks all connections from the host list and sets the current
         * master.
         */
        void _check();

        /**
         * Add array of hosts to host list. Doesn't do anything if hosts are
         * already in host list.
         * @param hostList the list of hosts to add
         * @param changed if new hosts were added
         */
        void _checkHosts(const BSONObj& hostList, bool& changed);

        /**
         * Updates host list.
         * Invariant: if nodesOffset is >= 0, _nodes[nodesOffset].conn should be
         *  equal to conn.
         *
         * @param conn the connection to check
         * @param maybePrimary OUT
         * @param verbose
         * @param nodesOffset - offset into _nodes array, -1 for not in it
         *
         * @return true if the connection is good or false if invariant
         *   is broken
         */
        bool _checkConnection( DBClientConnection* conn, string& maybePrimary,
                bool verbose, int nodesOffset );

        /**
         * Save the seed list for the current set into the _seedServers map
         * Should only be called if you're already holding _setsLock and this
         * monitor's _lock.
         */
        void _cacheServerAddresses_inlock();

        string _getServerAddress_inlock() const;

        NodeDiff _getHostDiff_inlock( const BSONObj& hostList );
        bool _shouldChangeHosts( const BSONObj& hostList, bool inlock );

        /**
         * @return the index to _nodes corresponding to the server address.
         */
        int _find( const string& server ) const ;
        int _find_inlock( const string& server ) const ;

        /**
         * Checks whether the given connection matches the connection stored in _nodes.
         * Mainly used for sanity checking to confirm that nodeOffset still
         * refers to the right connection after releasing and reacquiring
         * a mutex.
         */
        bool _checkConnMatch_inlock( DBClientConnection* conn, size_t nodeOffset ) const;

        /**
         * Populates the local view of the set using the list of servers.
         *
         * Invariants:
         * 1. Should be called while holding _setsLock and while not holding _lock since
         *    this calls #_checkConnection, which locks _checkConnectionLock
         * 2. _nodes should be empty before this is called
         */
        void _populateHosts_inSetsLock(const std::vector<HostAndPort>& seedList);

        // protects _localThresholdMillis, _nodes and refs to _nodes
        // (eg. _master & _lastReadPrefHost)
        mutable mongo::mutex _lock;

        /**
         * "Synchronizes" the _checkConnection method. Should ideally be one mutex per
         * connection object being used. The purpose of this lock is to make sure that
         * the reply from the connection the lock holder got is the actual response
         * to what it sent.
         *
         * Deadlock WARNING: never acquire this while holding _lock
         */
        mutable mongo::mutex  _checkConnectionLock;

        string _name;

        /**
         * Host list.
         */
        std::vector<Node> _nodes;
        int _master; // which node is the current master.  -1 means no master is known
        int _nextSlave; // which node is the current slave, only used by the deprecated getSlave

        // last host returned by _selectNode, used for round robin selection
        HostAndPort _lastReadPrefHost;

        // The number of consecutive times the set has been checked and every member in the set was down.
        int _failedChecks;

        static mongo::mutex _setsLock; // protects _seedServers and _sets

        // set name to seed list.
        // Used to rebuild the monitor if it is cleaned up but then the set is accessed again.
        static map<string, vector<HostAndPort> > _seedServers;
        static map<string, ReplicaSetMonitorPtr> _sets; // set name to Monitor

        static ConfigChangeHook _hook;
        int _localThresholdMillis; // local ping latency threshold (protected by _lock)

        static int _maxFailedChecks;
    };

    /** Use this class to connect to a replica set of servers.  The class will manage
       checking for which server in a replica set is master, and do failover automatically.

       This can also be used to connect to replica pairs since pairs are a subset of sets

       On a failover situation, expect at least one operation to return an error (throw
       an exception) before the failover is complete.  Operations are not retried.
    */
    class DBClientReplicaSet : public DBClientBase {
    public:
        using DBClientBase::query;
        using DBClientBase::update;
        using DBClientBase::remove;

        /** Call connect() after constructing. autoReconnect is always on for DBClientReplicaSet connections. */
        DBClientReplicaSet( const string& name , const vector<HostAndPort>& servers, double so_timeout=0 );
        virtual ~DBClientReplicaSet();

        /**
         * Returns false if no member of the set were reachable. This object
         * can still be used even when false was returned as it will try to
         * reconnect when you use it later.
         */
        bool connect();

        /**
         * Logs out the connection for the given database.
         *
         * @param dbname the database to logout from.
         * @param info the result object for the logout command (provided for backwards
         *     compatibility with mongo shell)
         */
        virtual void logout(const string& dbname, BSONObj& info);

        // ----------- simple functions --------------

        /** throws userassertion "no master found" */
        virtual auto_ptr<DBClientCursor> query(const string &ns, Query query, int nToReturn = 0, int nToSkip = 0,
                                               const BSONObj *fieldsToReturn = 0, int queryOptions = 0 , int batchSize = 0 );

        /** throws userassertion "no master found" */
        virtual BSONObj findOne(const string &ns, const Query& query, const BSONObj *fieldsToReturn = 0, int queryOptions = 0);

        virtual void insert( const string &ns , BSONObj obj , int flags=0);

        /** insert multiple objects.  Note that single object insert is asynchronous, so this version
            is only nominally faster and not worth a special effort to try to use.  */
        virtual void insert( const string &ns, const vector< BSONObj >& v , int flags=0);

        virtual void remove( const string &ns , Query obj , int flags );

        virtual void update( const string &ns , Query query , BSONObj obj , int flags );

        virtual void killCursor( long long cursorID );

        // ---- access raw connections ----

        /**
         * WARNING: this method is very dangerous - this object can decide to free the
         *     returned master connection any time.
         *
         * @return the reference to the address that points to the master connection.
         */
        DBClientConnection& masterConn();

        /**
         * WARNING: this method is very dangerous - this object can decide to free the
         *     returned master connection any time. This can also unpin the cached
         *     slaveOk/read preference connection.
         *
         * @return the reference to the address that points to a secondary connection.
         */
        DBClientConnection& slaveConn();

        // ---- callback pieces -------

        virtual void say( Message &toSend, bool isRetry = false , string* actualServer = 0);
        virtual bool recv( Message &toRecv );
        virtual void checkResponse( const char* data, int nReturned, bool* retry = NULL, string* targetHost = NULL );

        /* this is the callback from our underlying connections to notify us that we got a "not master" error.
         */
        void isntMaster();

        /* this is used to indicate we got a "not master or secondary" error from a secondary.
         */
        void isntSecondary();

        // ----- status ------

        virtual bool isFailed() const { return ! _master || _master->isFailed(); }
        bool isStillConnected();

        // ----- informational ----

        double getSoTimeout() const { return _so_timeout; }

        string toString() { return getServerAddress(); }

        string getServerAddress() const;

        virtual ConnectionString::ConnectionType type() const { return ConnectionString::SET; }
        virtual bool lazySupported() const { return true; }

        // ---- low level ------

        virtual bool call( Message &toSend, Message &response, bool assertOk=true , string * actualServer = 0 );
        virtual bool callRead( Message& toSend , Message& response ) { return checkMaster()->callRead( toSend , response ); }


    protected:
        /** Authorize.  Authorizes all nodes as needed
        */
        virtual void _auth(const BSONObj& params);

        virtual void sayPiggyBack( Message &toSend ) { checkMaster()->say( toSend ); }

    private:
        /**
         * Used to simplify slave-handling logic on errors
         *
         * @return back the passed cursor
         * @throws DBException if the directed node cannot accept the query because it
         *     is not a master
         */
        auto_ptr<DBClientCursor> checkSlaveQueryResult( auto_ptr<DBClientCursor> result );

        DBClientConnection * checkMaster();

        /**
         * Helper method for selecting a node based on the read preference. Will advance
         * the tag tags object if it cannot find a node that matches the current tag.
         *
         * @param readPref the preference to use for selecting a node.
         *
         * @return a pointer to the new connection object if it can find a good connection.
         *     Otherwise it returns NULL.
         *
         * @throws DBException when an error occurred either when trying to connect to
         *     a node that was thought to be ok or when an assertion happened.
         */
        DBClientConnection* selectNodeUsingTags(shared_ptr<ReadPreferenceSetting> readPref);

        /**
         * @return true if the last host used in the last slaveOk query is still in the
         * set and can be used for the given read preference.
         */
        bool checkLastHost(const ReadPreferenceSetting* readPref);

        /**
         * Destroys all cached information about the last slaveOk operation.
         */
        void invalidateLastSlaveOkCache();

        void _auth( DBClientConnection * conn );

        /**
         * Maximum number of retries to make for auto-retry logic when performing a slave ok
         * operation.
         */
        static const size_t MAX_RETRY;

        // Throws a DBException if the monitor doesn't exist and there isn't a cached seed to use.
        ReplicaSetMonitorPtr _getMonitor() const;

        string _setName;

        HostAndPort _masterHost;
        // Note: reason why this is a shared_ptr is because we want _lastSlaveOkConn to
        // keep a reference of the _master connection when it selected a primary node.
        // This is because the primary connection is special in mongos - it is the only
        // connection that is versioned.
        // WARNING: do not assign this variable (which will increment the internal ref
        // counter) to any other variable other than _lastSlaveOkConn.
        boost::shared_ptr<DBClientConnection> _master;

        // Last used host in a slaveOk query (can be a primary)
        HostAndPort _lastSlaveOkHost;
        // Last used connection in a slaveOk query (can be a primary)
        boost::shared_ptr<DBClientConnection> _lastSlaveOkConn;
        boost::shared_ptr<ReadPreferenceSetting> _lastReadPref;
        
        double _so_timeout;

        // we need to store so that when we connect to a new node on failure
        // we can re-auth
        // this could be a security issue, as the password is stored in memory
        // not sure if/how we should handle
        std::map<string, BSONObj> _auths; // dbName -> auth parameters

    protected:

        /**
         * for storing (non-threadsafe) information between lazy calls
         */
        class LazyState {
        public:
            LazyState() : _lastClient( NULL ), _lastOp( -1 ), _slaveOk( false ), _retries( 0 ) {}
            DBClientConnection* _lastClient;
            int _lastOp;
            bool _slaveOk;
            int _retries;

        } _lazyState;

    };

    /**
     * A simple object for representing the list of tags. The initial state will
     * have a valid current tag as long as the list is not empty.
     */
    class TagSet {
    public:
        /**
         * Creates an empty tag list that is initially exhausted.
         */
        TagSet();

        /**
         * Creates a copy of the given TagSet. The new copy will have the
         * iterator pointing at the initial position.
         */
        explicit TagSet(const TagSet& other);

        /**
         * Creates a tag set object that lazily iterates over the tag list.
         *
         * @param tags the list of tags associated with this option. This object
         *     will get a shared copy of the list. Therefore, it is important
         *     for the the given tag to live longer than the created tag set.
         */
        explicit TagSet(const BSONArray& tags);

        /**
         * Advance to the next tag.
         *
         * @throws AssertionException if iterator is exhausted before this is called.
         */
        void next();

        /**
         * Rests the iterator to point to the first element (if there is a tag).
         */
        void reset();

        //
        // Getters
        //

        /**
         * @return the current tag. Returned tag is invalid if isExhausted is true.
         */
        const BSONObj& getCurrentTag() const;

        /**
         * @return true if the iterator has been exhausted.
         */
        bool isExhausted() const;

        /**
         * @return an unordered iterator to the tag list. The caller is responsible for
         *     destroying the returned iterator.
         */
        BSONObjIterator* getIterator() const;

        /**
         * @returns true if the other TagSet has the same tag set specification with
         *     this tag set, disregarding where the current iterator is pointing to.
         */
        bool equals(const TagSet& other) const;

        const BSONArray& getTagBSON() const;

    private:
        /**
         * This is purposely undefined as the semantics for assignment can be
         * confusing. This is because BSONArrayIteratorSorted shouldn't be
         * copied (because of how it manages internal buffer).
         */
        TagSet& operator=(const TagSet& other);
        BSONObj _currentTag;
        bool _isExhausted;

        // Important: do not re-order _tags & _tagIterator
        BSONArray _tags;
        scoped_ptr<BSONArrayIteratorSorted> _tagIterator;
    };

    struct ReadPreferenceSetting {
        /**
         * @parm pref the read preference mode.
         * @param tag the tag set. Note that this object will have the
         *     tag set will have this in a reset state (meaning, this
         *     object's copy of tag will have the iterator in the initial
         *     position).
         */
        ReadPreferenceSetting(ReadPreference pref, const TagSet& tag):
            pref(pref), tags(tag) {
        }

        inline bool equals(const ReadPreferenceSetting& other) const {
            return pref == other.pref && tags.equals(other.tags);
        }

        BSONObj toBSON() const;

        const ReadPreference pref;
        TagSet tags;
    };
}
