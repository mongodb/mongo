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

#include "mongo/pch.h"

#include "mongo/client/dbclient_rs.h"

#include <fstream>
#include <memory>

#include "mongo/base/init.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/mutex.h" // for StaticObserver
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

namespace mongo {

    /*
     * Set of commands that can be used with $readPreference
     */
    set<string> _secOkCmdList;

    class PopulateReadPrefSecOkCmdList {
    public:
        PopulateReadPrefSecOkCmdList() {
            _secOkCmdList.insert("aggregate");
            _secOkCmdList.insert("collStats");
            _secOkCmdList.insert("count");
            _secOkCmdList.insert("distinct");
            _secOkCmdList.insert("dbStats");
            _secOkCmdList.insert("geoNear");
            _secOkCmdList.insert("geoSearch");
            _secOkCmdList.insert("geoWalk");
            _secOkCmdList.insert("group");
        }
    } _populateReadPrefSecOkCmdList;

    /**
     * Extracts the read preference settings from the query document. Note that this method
     * assumes that the query is ok for secondaries so it defaults to
     * ReadPreference_SecondaryPreferred when nothing is specified. Supports the following
     * format:
     *
     * Format A (official format):
     * { query: <actual query>, $readPreference: <read pref obj> }
     *
     * Format B (unofficial internal format from mongos):
     * { <actual query>, $queryOptions: { $readPreference: <read pref obj> }}
     *
     * @param query the raw query document
     *
     * @return the read preference setting if a read preference exists, otherwise the default read
     *         preference of Primary_Only. If the tags field was not present, it will contain one
     *         empty tag document {} which matches any tag.
     *
     * @throws AssertionException if the read preference object is malformed
     */
    ReadPreferenceSetting* _extractReadPref(const BSONObj& query, int queryOptions) {

        if (Query::hasReadPreference(query)) {

            ReadPreference pref = mongo::ReadPreference_SecondaryPreferred;

            BSONElement readPrefElement;

            if (query.hasField(Query::ReadPrefField.name())) {
                readPrefElement = query[Query::ReadPrefField.name()];
            }
            else {
                readPrefElement = query["$queryOptions"][Query::ReadPrefField.name()];
            }

            uassert(16381, "$readPreference should be an object",
                    readPrefElement.isABSONObj());
            const BSONObj& prefDoc = readPrefElement.Obj();

            uassert(16382, "mode not specified for read preference",
                    prefDoc.hasField(Query::ReadPrefModeField.name()));

            const string mode = prefDoc[Query::ReadPrefModeField.name()].String();

            if (mode == "primary") {
                pref = mongo::ReadPreference_PrimaryOnly;
            }
            else if (mode == "primaryPreferred") {
                pref = mongo::ReadPreference_PrimaryPreferred;
            }
            else if (mode == "secondary") {
                pref = mongo::ReadPreference_SecondaryOnly;
            }
            else if (mode == "secondaryPreferred") {
                pref = mongo::ReadPreference_SecondaryPreferred;
            }
            else if (mode == "nearest") {
                pref = mongo::ReadPreference_Nearest;
            }
            else {
                uasserted(16383, str::stream() << "Unknown read preference mode: " << mode);
            }

            if (prefDoc.hasField(Query::ReadPrefTagsField.name())) {
                const BSONElement& tagsElem = prefDoc[Query::ReadPrefTagsField.name()];
                uassert(16385, "tags for read preference should be an array",
                        tagsElem.type() == mongo::Array);

                TagSet tags(BSONArray(tagsElem.Obj().getOwned()));
                if (pref == mongo::ReadPreference_PrimaryOnly && !tags.isExhausted()) {
                    uassert(16384, "Only empty tags are allowed with primary read preference",
                            tags.getCurrentTag().isEmpty());
                }

                return new ReadPreferenceSetting(pref, tags);
            }
            else {
                TagSet tags(BSON_ARRAY(BSONObj()));
                return new ReadPreferenceSetting(pref, tags);
            }
        }

        // Default read pref is primary only or secondary preferred with slaveOK
        TagSet tags(BSON_ARRAY(BSONObj()));
        ReadPreference pref =
            queryOptions & QueryOption_SlaveOk ?
                mongo::ReadPreference_SecondaryPreferred : mongo::ReadPreference_PrimaryOnly;
        return new ReadPreferenceSetting(pref, tags);
    }

    // --------------------------------
    // ----- DBClientReplicaSet ---------
    // --------------------------------

    const size_t DBClientReplicaSet::MAX_RETRY = 3;

    DBClientReplicaSet::DBClientReplicaSet( const string& name , const vector<HostAndPort>& servers, double so_timeout )
        : _setName( name ), _so_timeout( so_timeout ) {
        ReplicaSetMonitor::createIfNeeded( name, servers );
    }

    DBClientReplicaSet::~DBClientReplicaSet() {
    }

    ReplicaSetMonitorPtr DBClientReplicaSet::_getMonitor() const {
        ReplicaSetMonitorPtr rsm = ReplicaSetMonitor::get( _setName, true );
        // If you can't get a ReplicaSetMonitor then this connection isn't valid
        uassert( 16340, str::stream() << "No replica set monitor active and no cached seed "
                 "found for set: " << _setName, rsm );
        return rsm;
    }

    // This can't throw an exception because it is called in the destructor of ScopedDbConnection
    string DBClientReplicaSet::getServerAddress() const {
        ReplicaSetMonitorPtr rsm = ReplicaSetMonitor::get( _setName, true );
        if ( !rsm ) {
            warning() << "Trying to get server address for DBClientReplicaSet, but no "
                "ReplicaSetMonitor exists for " << _setName << endl;
            return str::stream() << _setName << "/" ;
        }
        return rsm->getServerAddress();
    }

    void DBClientReplicaSet::setRunCommandHook(DBClientWithCommands::RunCommandHookFunc func) {
        // Set the hooks in both our sub-connections and in ourselves.
        if (_master) {
            _master->setRunCommandHook(func);
        }
        if (_lastSlaveOkConn) {
           _lastSlaveOkConn->setRunCommandHook(func);
        }
        _runCommandHook = func;
    }

    // A replica set connection is never disconnected, since it controls its own reconnection
    // logic.
    //
    // Has the side effect of proactively clearing any cached connections which have been
    // disconnected in the background.
    bool DBClientReplicaSet::isStillConnected() {

        if ( _master && !_master->isStillConnected() ) {
            _master.reset();
            _masterHost = HostAndPort();
            // Don't notify monitor of bg failure, since it's not clear how long ago it happened
        }

        if ( _lastSlaveOkConn && !_lastSlaveOkConn->isStillConnected() ) {
            _lastSlaveOkConn.reset();
            _lastSlaveOkHost = HostAndPort();
            // Reset read pref too, since we're re-selecting the slaveOk host anyway
            _lastReadPref.reset();
            // Don't notify monitor of bg failure, since it's not clear how long ago it happened
        }

        return true;
    }

    // Internal implementation of isSecondaryQuery, takes previously-parsed read preference
    static bool _isSecondaryQuery( const string& ns,
                                   const BSONObj& queryObj,
                                   const ReadPreferenceSetting& readPref ) {

        // If the read pref is primary only, this is not a secondary query
        if (readPref.pref == ReadPreference_PrimaryOnly) return false;

        if (ns.find(".$cmd") == string::npos) {
            return true;
        }

        // This is a command with secondary-possible read pref
        // Only certain commands are supported for secondary operation.

        BSONObj actualQueryObj;
        if (strcmp(queryObj.firstElement().fieldName(), "query") == 0) {
            actualQueryObj = queryObj["query"].embeddedObject();
        }
        else {
            actualQueryObj = queryObj;
        }

        const string cmdName = actualQueryObj.firstElementFieldName();
        if (_secOkCmdList.count(cmdName) == 1) {
            return true;
        }

        if (cmdName == "mapReduce" || cmdName == "mapreduce") {
            if (!actualQueryObj.hasField("out")) {
                return false;
            }

            BSONElement outElem(actualQueryObj["out"]);
            if (outElem.isABSONObj() && outElem["inline"].trueValue()) {
                return true;
            }
        }

        return false;
    }

    bool DBClientReplicaSet::isSecondaryQuery( const string& ns,
                                               const BSONObj& queryObj,
                                               int queryOptions ) {
        auto_ptr<ReadPreferenceSetting> readPref( _extractReadPref( queryObj, queryOptions ) );
        return _isSecondaryQuery( ns, queryObj, *readPref );
    }

    DBClientConnection * DBClientReplicaSet::checkMaster() {
        ReplicaSetMonitorPtr monitor = _getMonitor();
        HostAndPort h = monitor->getMaster();

        if ( h == _masterHost && _master ) {
            // a master is selected.  let's just make sure connection didn't die
            if ( ! _master->isFailed() )
                return _master.get();
            monitor->notifyFailure( _masterHost );
        }

        _masterHost = monitor->getMaster();

        ConnectionString connStr(_masterHost);

        string errmsg;
        DBClientConnection* newConn = NULL;

        try {
            // Needs to perform a dynamic_cast because we need to set the replSet
            // callback. We should eventually not need this after we remove the
            // callback.
            newConn = dynamic_cast<DBClientConnection*>(
                    connStr.connect(errmsg, _so_timeout));
        }
        catch (const AssertionException& ex) {
            errmsg = ex.toString();
        }

        if (newConn == NULL || !errmsg.empty()) {
            monitor->notifyFailure(_masterHost);
            uasserted(13639, str::stream() << "can't connect to new replica set master ["
                      << _masterHost.toString() << "]"
                      << (errmsg.empty()? "" : ", err: ") << errmsg);
        }

        _master.reset(newConn);
        _master->setReplSetClientCallback(this);
        _master->setRunCommandHook(_runCommandHook);

        _auth( _master.get() );
        return _master.get();
    }

    bool DBClientReplicaSet::checkLastHost(const ReadPreferenceSetting* readPref) {
        if (_lastSlaveOkHost.empty()) {
            return false;
        }

        ReplicaSetMonitorPtr monitor = _getMonitor();

        if (_lastSlaveOkConn && _lastSlaveOkConn->isFailed()) {
            invalidateLastSlaveOkCache();
            return false;
        }

        return _lastSlaveOkConn && _lastReadPref && _lastReadPref->equals(*readPref);
    }

    void DBClientReplicaSet::_auth( DBClientConnection * conn ) {
        for (map<string, BSONObj>::const_iterator i = _auths.begin(); i != _auths.end(); ++i) {
            try {
                conn->auth(i->second);
            }
            catch (const UserException&) {
                warning() << "cached auth failed for set: " << _setName <<
                    " db: " << i->second[saslCommandUserDBFieldName].str() <<
                    " user: " << i->second[saslCommandUserFieldName].str() << endl;
            }
        }
    }

    DBClientConnection& DBClientReplicaSet::masterConn() {
        return *checkMaster();
    }

    DBClientConnection& DBClientReplicaSet::slaveConn() {
        BSONArray emptyArray(BSON_ARRAY(BSONObj()));
        TagSet tags(emptyArray);
        shared_ptr<ReadPreferenceSetting> readPref(
                new ReadPreferenceSetting(ReadPreference_SecondaryPreferred, tags));
        DBClientConnection* conn = selectNodeUsingTags(readPref);

        uassert( 16369, str::stream() << "No good nodes available for set: "
                 << _getMonitor()->getName(), conn != NULL );

        return *conn;
    }

    bool DBClientReplicaSet::connect() {
        return _getMonitor()->isAnyNodeOk();
    }

    static bool isAuthenticationException( const DBException& ex ) {
        return ex.getCode() == ErrorCodes::AuthenticationFailed;
    }

    void DBClientReplicaSet::_auth( const BSONObj& params ) {

        // We prefer to authenticate against a primary, but otherwise a secondary is ok too
        // Empty tag matches every secondary
        TagSet tags(BSON_ARRAY(BSONObj()));
        shared_ptr<ReadPreferenceSetting> readPref(
            new ReadPreferenceSetting( ReadPreference_PrimaryPreferred, tags ) );

        LOG(3) << "dbclient_rs authentication of " << _getMonitor()->getName() << endl;

        // NOTE that we retry MAX_RETRY + 1 times, since we're always primary preferred we don't
        // fallback to the primary.
        Status lastNodeStatus = Status::OK();
        for ( size_t retry = 0; retry < MAX_RETRY + 1; retry++ ) {
            try {
                DBClientConnection* conn = selectNodeUsingTags( readPref );

                if ( conn == NULL ) {
                    break;
                }

                conn->auth( params );

                // Cache the new auth information since we now validated it's good
                _auths[params[saslCommandUserDBFieldName].str()] = params.getOwned();

                // Ensure the only child connection open is the one we authenticated against - other
                // child connections may not have full authentication information.
                // NOTE: _lastSlaveOkConn may or may not be the same as _master
                dassert(_lastSlaveOkConn.get() == conn || _master.get() == conn);
                if ( conn != _lastSlaveOkConn.get() ) {
                    _lastSlaveOkHost = HostAndPort();
                    _lastSlaveOkConn.reset();
                }
                if ( conn != _master.get() ) {
                    _masterHost = HostAndPort();
                    _master.reset();
                }

                return;
            }
            catch ( const DBException &ex ) {

                // We care if we can't authenticate (i.e. bad password) in credential params.
                if ( isAuthenticationException( ex ) ) {
                    throw;
                }

                StringBuilder errMsgB;
                errMsgB << "can't authenticate against replica set node "
                        << _lastSlaveOkHost.toString();
                lastNodeStatus = ex.toStatus( errMsgB.str() );

                LOG(1) << lastNodeStatus.reason() << endl;
                invalidateLastSlaveOkCache();
            }
        }

        if ( lastNodeStatus.isOK() ) {
            StringBuilder assertMsgB;
            assertMsgB << "Failed to authenticate, no good nodes in " << _getMonitor()->getName();
            uasserted( ErrorCodes::NodeNotFound, assertMsgB.str() );
        }
        else {
            uasserted( lastNodeStatus.code(), lastNodeStatus.reason() );
        }
    }

    void DBClientReplicaSet::logout(const string &dbname, BSONObj& info) {
        DBClientConnection* priConn = checkMaster();

        priConn->logout(dbname, info);
        _auths.erase(dbname);

        /* Also logout the cached secondary connection. Note that this is only
         * needed when we actually have something cached and is last known to be
         * working.
         */
        if (_lastSlaveOkConn.get() != NULL && !_lastSlaveOkConn->isFailed()) {
            try {
                BSONObj dummy;
                _lastSlaveOkConn->logout(dbname, dummy);
            }
            catch (const DBException&) {
                // Make sure we can't use this connection again.
                verify(_lastSlaveOkConn->isFailed());
            }
        }
    }

    // ------------- simple functions -----------------

    void DBClientReplicaSet::insert( const string &ns , BSONObj obj , int flags) {
        checkMaster()->insert(ns, obj, flags);
    }

    void DBClientReplicaSet::insert( const string &ns, const vector< BSONObj >& v , int flags) {
        checkMaster()->insert(ns, v, flags);
    }

    void DBClientReplicaSet::remove( const string &ns , Query obj , int flags ) {
        checkMaster()->remove(ns, obj, flags);
    }

    void DBClientReplicaSet::update( const string &ns , Query query , BSONObj obj , int flags ) {
        return checkMaster()->update( ns, query, obj, flags );
    }

    auto_ptr<DBClientCursor> DBClientReplicaSet::query(const string &ns,
                                                       Query query,
                                                       int nToReturn,
                                                       int nToSkip,
                                                       const BSONObj *fieldsToReturn,
                                                       int queryOptions,
                                                       int batchSize) {

        shared_ptr<ReadPreferenceSetting> readPref( _extractReadPref( query.obj, queryOptions ) );
        if ( _isSecondaryQuery( ns, query.obj, *readPref ) ) {

            LOG( 3 ) << "dbclient_rs query using secondary or tagged node selection in "
                                << _getMonitor()->getName() << ", read pref is "
                                << readPref->toBSON() << " (primary : "
                                << ( _master.get() != NULL ?
                                        _master->getServerAddress() : "[not cached]" )
                                << ", lastTagged : "
                                << ( _lastSlaveOkConn.get() != NULL ?
                                        _lastSlaveOkConn->getServerAddress() : "[not cached]" )
                                << ")" << endl;

            string lastNodeErrMsg;
            for (size_t retry = 0; retry < MAX_RETRY; retry++) {
                try {
                    DBClientConnection* conn = selectNodeUsingTags(readPref);

                    if (conn == NULL) {
                        break;
                    }

                    auto_ptr<DBClientCursor> cursor = conn->query(ns, query,
                            nToReturn, nToSkip, fieldsToReturn, queryOptions,
                            batchSize);

                    return checkSlaveQueryResult(cursor);
                }
                catch (const DBException &dbExcep) {
                    StringBuilder errMsgBuilder;
                    errMsgBuilder << "can't query replica set node "
                               << _lastSlaveOkHost.toString() << ": " << causedBy( dbExcep );
                    lastNodeErrMsg = errMsgBuilder.str();

                    LOG(1) << lastNodeErrMsg << endl;
                    invalidateLastSlaveOkCache();
                }
            }

            StringBuilder assertMsg;
            assertMsg << "Failed to do query, no good nodes in " << _getMonitor()->getName();
            if ( !lastNodeErrMsg.empty() ) {
                assertMsg << ", last error: " << lastNodeErrMsg;
            }

            uasserted( 16370, assertMsg.str() );
        }

        LOG( 3 ) << "dbclient_rs query to primary node in " << _getMonitor()->getName() << endl;

        return checkMaster()->query(ns, query, nToReturn, nToSkip, fieldsToReturn,
                queryOptions, batchSize);
    }

    BSONObj DBClientReplicaSet::findOne(const string &ns,
                                        const Query& query,
                                        const BSONObj *fieldsToReturn,
                                        int queryOptions) {

        shared_ptr<ReadPreferenceSetting> readPref( _extractReadPref( query.obj, queryOptions ) );
        if ( _isSecondaryQuery( ns, query.obj, *readPref ) ) {

            LOG( 3 ) << "dbclient_rs findOne using secondary or tagged node selection in "
                                << _getMonitor()->getName() << ", read pref is "
                                << readPref->toBSON() << " (primary : "
                                << ( _master.get() != NULL ?
                                        _master->getServerAddress() : "[not cached]" )
                                << ", lastTagged : "
                                << ( _lastSlaveOkConn.get() != NULL ?
                                        _lastSlaveOkConn->getServerAddress() : "[not cached]" )
                                << ")" << endl;

            string lastNodeErrMsg;

            for (size_t retry = 0; retry < MAX_RETRY; retry++) {
                try {
                    DBClientConnection* conn = selectNodeUsingTags(readPref);

                    if (conn == NULL) {
                        break;
                    }

                    return conn->findOne(ns,query,fieldsToReturn,queryOptions);
                }
                catch ( const DBException &dbExcep ) {
                    StringBuilder errMsgBuilder;
                    errMsgBuilder << "can't findone replica set node "
                               << _lastSlaveOkHost.toString() << ": " << causedBy( dbExcep );
                    lastNodeErrMsg = errMsgBuilder.str();

                    LOG(1) << lastNodeErrMsg << endl;
                    invalidateLastSlaveOkCache();
                }
            }

            StringBuilder assertMsg;
            assertMsg << "Failed to call findOne, no good nodes in " << _getMonitor()->getName();
            if ( !lastNodeErrMsg.empty() ) {
                assertMsg << ", last error: " << lastNodeErrMsg;
            }

            uasserted(16379, assertMsg.str());
        }

        LOG( 3 ) << "dbclient_rs findOne to primary node in " << _getMonitor()->getName()
                            << endl;

        return checkMaster()->findOne(ns,query,fieldsToReturn,queryOptions);
    }

    void DBClientReplicaSet::killCursor( long long cursorID ) {
        // we should never call killCursor on a replica set connection
        // since we don't know which server it belongs to
        // can't assume master because of slave ok
        // and can have a cursor survive a master change
        verify(0);
    }

    void DBClientReplicaSet::isntMaster() { 
        log() << "got not master for: " << _masterHost << endl;
        // Can't use _getMonitor because that will create a new monitor from the cached seed if
        // the monitor doesn't exist.
        ReplicaSetMonitorPtr monitor = ReplicaSetMonitor::get( _setName );
        if ( monitor ) {
            monitor->notifyFailure( _masterHost );
        }
        _master.reset(); 
    }

    auto_ptr<DBClientCursor> DBClientReplicaSet::checkSlaveQueryResult( auto_ptr<DBClientCursor> result ){
        if ( result.get() == NULL ) return result;

        BSONObj error;
        bool isError = result->peekError( &error );
        if( ! isError ) return result;

        // We only check for "not master or secondary" errors here

        // If the error code here ever changes, we need to change this code also
        BSONElement code = error["code"];
        if( code.isNumber() &&
            code.Int() == NotMasterOrSecondaryCode /* not master or secondary */ ) {
            isntSecondary();
            throw DBException( str::stream() << "slave " << _lastSlaveOkHost.toString()
                    << " is no longer secondary", 14812 );
        }

        return result;
    }

    void DBClientReplicaSet::isntSecondary() {
        log() << "slave no longer has secondary status: " << _lastSlaveOkHost << endl;
        // Failover to next slave
        _getMonitor()->notifySlaveFailure( _lastSlaveOkHost );
        _lastSlaveOkConn.reset();
    }

    DBClientConnection* DBClientReplicaSet::selectNodeUsingTags(
            shared_ptr<ReadPreferenceSetting> readPref) {
        if (checkLastHost(readPref.get())) {

            LOG( 3 ) << "dbclient_rs selecting compatible last used node " << _lastSlaveOkHost
                                << endl;

            return _lastSlaveOkConn.get();
        }

        ReplicaSetMonitorPtr monitor = _getMonitor();
        bool isPrimarySelected = false;
        _lastSlaveOkHost = monitor->selectAndCheckNode(readPref->pref, &readPref->tags,
                &isPrimarySelected);

        if ( _lastSlaveOkHost.empty() ){

            LOG( 3 ) << "dbclient_rs no compatible node found" << endl;

            return NULL;
        }

        _lastReadPref = readPref;

        // Primary connection is special because it is the only connection that is
        // versioned in mongos. Therefore, we have to make sure that this object
        // maintains only one connection to the primary and use that connection
        // every time we need to talk to the primary.
        if (isPrimarySelected) {
            checkMaster();
            _lastSlaveOkConn = _master;
            _lastSlaveOkHost = _masterHost; // implied, but still assign just to be safe

            LOG( 3 ) << "dbclient_rs selecting primary node " << _lastSlaveOkHost << endl;

            return _master.get();
        }

        string errmsg;
        ConnectionString connStr(_lastSlaveOkHost);
        // Needs to perform a dynamic_cast because we need to set the replSet
        // callback. We should eventually not need this after we remove the
        // callback.
        DBClientConnection* newConn = dynamic_cast<DBClientConnection*>(
                connStr.connect(errmsg, _so_timeout));

        // Assert here instead of returning NULL since the contract of this method is such
        // that returning NULL means none of the nodes were good, which is not the case here.
        uassert(16532, str::stream() << "Failed to connect to " << _lastSlaveOkHost.toString(),
                newConn != NULL);

        _lastSlaveOkConn.reset(newConn);
        _lastSlaveOkConn->setReplSetClientCallback(this);
        _lastSlaveOkConn->setRunCommandHook(_runCommandHook);

        _auth(_lastSlaveOkConn.get());

        LOG( 3 ) << "dbclient_rs selecting node " << _lastSlaveOkHost << endl;

        return _lastSlaveOkConn.get();
    }

    void DBClientReplicaSet::say(Message& toSend, bool isRetry, string* actualServer) {

        if (!isRetry)
            _lazyState = LazyState();

        const int lastOp = toSend.operation();

        if (lastOp == dbQuery) {
            // TODO: might be possible to do this faster by changing api
            DbMessage dm(toSend);
            QueryMessage qm(dm);

            shared_ptr<ReadPreferenceSetting> readPref( _extractReadPref( qm.query,
                                                                          qm.queryOptions ) );
            if ( _isSecondaryQuery( qm.ns, qm.query, *readPref ) ) {

                LOG( 3 ) << "dbclient_rs say using secondary or tagged node selection in "
                                    << _getMonitor()->getName() << ", read pref is "
                                    << readPref->toBSON() << " (primary : "
                                    << ( _master.get() != NULL ?
                                            _master->getServerAddress() : "[not cached]" )
                                    << ", lastTagged : "
                                    << ( _lastSlaveOkConn.get() != NULL ?
                                            _lastSlaveOkConn->getServerAddress() : "[not cached]" )
                                    << ")" << endl;

                string lastNodeErrMsg;

                for (size_t retry = 0; retry < MAX_RETRY; retry++) {
                    _lazyState._retries = retry;
                    try {
                        DBClientConnection* conn = selectNodeUsingTags(readPref);

                        if (conn == NULL) {
                            break;
                        }

                        if (actualServer != NULL) {
                            *actualServer = conn->getServerAddress();
                        }

                        conn->say(toSend);

                        _lazyState._lastOp = lastOp;
                        _lazyState._secondaryQueryOk = true;
                        _lazyState._lastClient = conn;
                    }
                    catch ( const DBException& DBExcep ) {
                        StringBuilder errMsgBuilder;
                        errMsgBuilder << "can't callLazy replica set node "
                                   << _lastSlaveOkHost.toString() << ": " << causedBy( DBExcep );
                        lastNodeErrMsg = errMsgBuilder.str();

                        LOG(1) << lastNodeErrMsg << endl;
                        invalidateLastSlaveOkCache();
                        continue;
                    }

                    return;
                }

                StringBuilder assertMsg;
                assertMsg << "Failed to call say, no good nodes in " << _getMonitor()->getName();
                if ( !lastNodeErrMsg.empty() ) {
                    assertMsg << ", last error: " << lastNodeErrMsg;
                }

                uasserted(16380, assertMsg.str());
            }
        }

        LOG( 3 ) << "dbclient_rs say to primary node in " << _getMonitor()->getName()
                            << endl;

        DBClientConnection* master = checkMaster();
        if (actualServer)
            *actualServer = master->getServerAddress();

        _lazyState._lastOp = lastOp;
        _lazyState._secondaryQueryOk = false;
        // Don't retry requests to primary since there is only one host to try
        _lazyState._retries = MAX_RETRY;
        _lazyState._lastClient = master;

        master->say(toSend);
        return;
    }

    bool DBClientReplicaSet::recv( Message& m ) {

        verify( _lazyState._lastClient );

        // TODO: It would be nice if we could easily wrap a conn error as a result error
        try {
            return _lazyState._lastClient->recv( m );
        }
        catch( DBException& e ){
            log() << "could not receive data from " << _lazyState._lastClient->toString() << causedBy( e ) << endl;
            return false;
        }
    }

    void DBClientReplicaSet::checkResponse( const char* data, int nReturned, bool* retry, string* targetHost ){

        // For now, do exactly as we did before, so as not to break things.  In general though, we
        // should fix this so checkResponse has a more consistent contract.
        if( ! retry ){
            if( _lazyState._lastClient )
                return _lazyState._lastClient->checkResponse( data, nReturned );
            else
                return checkMaster()->checkResponse( data, nReturned );
        }

        *retry = false;
        if( targetHost && _lazyState._lastClient ) *targetHost = _lazyState._lastClient->getServerAddress();
        else if (targetHost) *targetHost = "";

        if( ! _lazyState._lastClient ) return;

        // nReturned == 1 means that we got one result back, which might be an error
        // nReturned == -1 is a sentinel value for "no data returned" aka (usually) network problem
        // If neither, this must be a query result so our response is ok wrt the replica set
        if( nReturned != 1 && nReturned != -1 ) return;

        BSONObj dataObj;
        if( nReturned == 1 ) dataObj = BSONObj( data );

        // Check if we should retry here
        if( _lazyState._lastOp == dbQuery && _lazyState._secondaryQueryOk ){

            // query could potentially go to a secondary, so see if this is an error (or empty) and
            // retry if we're not past our retry limit.

            if( nReturned == -1 /* no result, maybe network problem */ ||
                ( hasErrField( dataObj ) &&  ! dataObj["code"].eoo()
                  && dataObj["code"].Int() == NotMasterOrSecondaryCode ) ){

                if( _lazyState._lastClient == _lastSlaveOkConn.get() ){
                    isntSecondary();
                }
                else if( _lazyState._lastClient == _master.get() ){
                    isntMaster();
                }
                else {
                    warning() << "passed " << dataObj << " but last rs client "
                              << _lazyState._lastClient->toString() << " is not master or secondary"
                              << endl;
                }

                if ( _lazyState._retries < static_cast<int>( MAX_RETRY ) ) {
                    _lazyState._retries++;
                    *retry = true;
                }
                else{
                    log() << "too many retries (" << _lazyState._retries
                          << "), could not get data from replica set" << endl;
                }
            }
        }
        else if( _lazyState._lastOp == dbQuery ){

            // if query could not potentially go to a secondary, just mark the master as bad

            if( nReturned == -1 /* no result, maybe network problem */ ||
               ( hasErrField( dataObj ) &&  ! dataObj["code"].eoo()
                 && dataObj["code"].Int() == NotMasterNoSlaveOkCode ) )
            {
                if( _lazyState._lastClient == _master.get() ){
                    isntMaster();
                }
            }
        }
    }


    bool DBClientReplicaSet::call(Message &toSend,
                                  Message &response,
                                  bool assertOk,
                                  string * actualServer) {
        const char * ns = 0;

        if (toSend.operation() == dbQuery) {
            // TODO: might be possible to do this faster by changing api
            DbMessage dm(toSend);
            QueryMessage qm(dm);
            ns = qm.ns;

            shared_ptr<ReadPreferenceSetting> readPref( _extractReadPref( qm.query,
                                                                          qm.queryOptions ) );
            if ( _isSecondaryQuery( ns, qm.query, *readPref ) ) {

                LOG( 3 ) << "dbclient_rs call using secondary or tagged node selection in "
                                    << _getMonitor()->getName() << ", read pref is "
                                    << readPref->toBSON() << " (primary : "
                                    << ( _master.get() != NULL ?
                                            _master->getServerAddress() : "[not cached]" )
                                    << ", lastTagged : "
                                    << ( _lastSlaveOkConn.get() != NULL ?
                                            _lastSlaveOkConn->getServerAddress() : "[not cached]" )
                                    << ")" << endl;

                for (size_t retry = 0; retry < MAX_RETRY; retry++) {
                    try {
                        DBClientConnection* conn = selectNodeUsingTags(readPref);

                        if (conn == NULL) {
                            return false;
                        }

                        if (actualServer != NULL) {
                            *actualServer = conn->getServerAddress();
                        }

                        return conn->call(toSend, response, assertOk);
                    }
                    catch ( const DBException& dbExcep ) {
                        LOG(1) << "can't call replica set node " << _lastSlaveOkHost << ": "
                                          << causedBy( dbExcep ) << endl;

                        if ( actualServer ) *actualServer = "";

                        invalidateLastSlaveOkCache();
                    }
                }

                // Was not able to successfully send after max retries
                return false;
            }
        }
        
        LOG( 3 ) << "dbclient_rs call to primary node in " << _getMonitor()->getName() << endl;

        DBClientConnection* m = checkMaster();
        if ( actualServer )
            *actualServer = m->getServerAddress();
        
        if ( ! m->call( toSend , response , assertOk ) )
            return false;

        if ( ns ) {
            QueryResult * res = (QueryResult*)response.singleData();
            if ( res->nReturned == 1 ) {
                BSONObj x(res->data() );
                if ( str::contains( ns , "$cmd" ) ) {
                    if ( isNotMasterErrorString( x["errmsg"] ) )
                        isntMaster();
                }
                else {
                    if ( isNotMasterErrorString( getErrField( x ) ) )
                        isntMaster();
                }
            }
        }

        return true;
    }

    void DBClientReplicaSet::invalidateLastSlaveOkCache() {
        /* This is not wrapped in with if (_lastSlaveOkConn && _lastSlaveOkConn->isFailed())
         * because there are certain exceptions that will not make the connection be labeled
         * as failed. For example, asserts 13079, 13080, 16386
         */
        _getMonitor()->notifySlaveFailure(_lastSlaveOkHost);
        _lastSlaveOkHost = HostAndPort();
        _lastSlaveOkConn.reset();
    }

    TagSet::TagSet():
            _isExhausted(true),
            _tagIterator(new BSONArrayIteratorSorted(_tags)) {
    }

    TagSet::TagSet(const TagSet& other) :
            _isExhausted(false),
            _tags(other._tags.getOwned()),
            _tagIterator(new BSONArrayIteratorSorted(_tags)) {
        next();
    }

    TagSet::TagSet(const BSONArray& tags) :
            _isExhausted(false),
            _tags(tags.getOwned()),
            _tagIterator(new BSONArrayIteratorSorted(_tags)) {
        next();
    }

    void TagSet::next() {
        if (_tagIterator->more()) {
            const BSONElement& nextTag = _tagIterator->next();
            uassert(16357, "Tags should be a BSON object", nextTag.isABSONObj());
            _currentTag = nextTag.Obj();
        }
        else {
            _isExhausted = true;
        }
    }

    void TagSet::reset() {
        _isExhausted = false;
        _tagIterator.reset(new BSONArrayIteratorSorted(_tags));
        next();
    }

    const BSONObj& TagSet::getCurrentTag() const {
        verify(!_isExhausted);
        return _currentTag;
    }

    bool TagSet::isExhausted() const {
        return _isExhausted;
    }

    BSONObjIterator* TagSet::getIterator() const {
        return new BSONObjIterator(_tags);
    }

    bool TagSet::equals(const TagSet& other) const {
        return _tags.equal(other._tags);
    }

    const BSONArray& TagSet::getTagBSON() const {
        return _tags;
    }

    string readPrefToString( ReadPreference pref ) {
        switch ( pref ) {
        case ReadPreference_PrimaryOnly:
            return "primary only";
        case ReadPreference_PrimaryPreferred:
            return "primary pref";
        case ReadPreference_SecondaryOnly:
            return "secondary only";
        case ReadPreference_SecondaryPreferred:
            return "secondary pref";
        case ReadPreference_Nearest:
            return "nearest";
        default:
            return "Unknown";
        }
    }

    BSONObj ReadPreferenceSetting::toBSON() const {
        BSONObjBuilder bob;
        bob.append( "pref", readPrefToString( pref ) );
        bob.append( "tags", tags.getTagBSON() );
        return bob.obj();
    }
}
