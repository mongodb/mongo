/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/dbclient_rs.h"

#include <memory>
#include <utility>

#include "mongo/bson/util/builder.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/util/log.h"

namespace mongo {

using std::shared_ptr;
using std::unique_ptr;
using std::endl;
using std::map;
using std::set;
using std::string;
using std::vector;

namespace {

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
        _secOkCmdList.insert("explain");
        _secOkCmdList.insert("find");
        _secOkCmdList.insert("geoNear");
        _secOkCmdList.insert("geoSearch");
        _secOkCmdList.insert("group");
    }
} _populateReadPrefSecOkCmdList;

/**
 * Extracts the read preference settings from the query document. Note that this method
 * assumes that the query is ok for secondaries so it defaults to
 * ReadPreference::SecondaryPreferred when nothing is specified. Supports the following
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
        BSONElement readPrefElement;

        if (query.hasField(Query::ReadPrefField.name())) {
            readPrefElement = query[Query::ReadPrefField.name()];
        } else {
            readPrefElement = query["$queryOptions"][Query::ReadPrefField.name()];
        }

        uassert(16381, "$readPreference should be an object", readPrefElement.isABSONObj());

        const BSONObj& prefDoc = readPrefElement.Obj();

        auto readPrefSetting = uassertStatusOK(ReadPreferenceSetting::fromBSON(prefDoc));

        return new ReadPreferenceSetting(std::move(readPrefSetting));
    }

    // Default read pref is primary only or secondary preferred with slaveOK
    ReadPreference pref = queryOptions & QueryOption_SlaveOk
        ? mongo::ReadPreference::SecondaryPreferred
        : mongo::ReadPreference::PrimaryOnly;
    return new ReadPreferenceSetting(pref, TagSet());
}
}  // namespace

// --------------------------------
// ----- DBClientReplicaSet ---------
// --------------------------------

const size_t DBClientReplicaSet::MAX_RETRY = 3;
bool DBClientReplicaSet::_authPooledSecondaryConn = true;

DBClientReplicaSet::DBClientReplicaSet(const string& name,
                                       const vector<HostAndPort>& servers,
                                       double so_timeout)
    : _setName(name), _so_timeout(so_timeout) {
    ReplicaSetMonitor::createIfNeeded(name, set<HostAndPort>(servers.begin(), servers.end()));
}

DBClientReplicaSet::~DBClientReplicaSet() {
    if (_lastSlaveOkConn.get() == _master.get()) {
        _lastSlaveOkConn.release();
    }
}

ReplicaSetMonitorPtr DBClientReplicaSet::_getMonitor() const {
    ReplicaSetMonitorPtr rsm = ReplicaSetMonitor::get(_setName);

    // If you can't get a ReplicaSetMonitor then this connection isn't valid
    uassert(16340,
            str::stream() << "No replica set monitor active and no cached seed "
                             "found for set: "
                          << _setName,
            rsm);
    return rsm;
}

// This can't throw an exception because it is called in the destructor of ScopedDbConnection
string DBClientReplicaSet::getServerAddress() const {
    ReplicaSetMonitorPtr rsm = ReplicaSetMonitor::get(_setName);
    if (!rsm) {
        warning() << "Trying to get server address for DBClientReplicaSet, but no "
                     "ReplicaSetMonitor exists for "
                  << _setName;
        return str::stream() << _setName << "/";
    }
    return rsm->getServerAddress();
}

HostAndPort DBClientReplicaSet::getSuspectedPrimaryHostAndPort() const {
    if (!_master) {
        return HostAndPort();
    }
    return _master->getServerHostAndPort();
}

void DBClientReplicaSet::setRequestMetadataWriter(rpc::RequestMetadataWriter writer) {
    // Set the hooks in both our sub-connections and in ourselves.
    if (_master) {
        _master->setRequestMetadataWriter(writer);
    }
    if (_lastSlaveOkConn.get()) {
        _lastSlaveOkConn->setRequestMetadataWriter(writer);
    }
    DBClientWithCommands::setRequestMetadataWriter(std::move(writer));
}

void DBClientReplicaSet::setReplyMetadataReader(rpc::ReplyMetadataReader reader) {
    // Set the hooks in both our sub-connections and in ourselves.
    if (_master) {
        _master->setReplyMetadataReader(reader);
    }
    if (_lastSlaveOkConn.get()) {
        _lastSlaveOkConn->setReplyMetadataReader(reader);
    }
    DBClientWithCommands::setReplyMetadataReader(std::move(reader));
}

int DBClientReplicaSet::getMinWireVersion() {
    return _getMonitor()->getMinWireVersion();
}

int DBClientReplicaSet::getMaxWireVersion() {
    return _getMonitor()->getMaxWireVersion();
}

// A replica set connection is never disconnected, since it controls its own reconnection
// logic.
//
// Has the side effect of proactively clearing any cached connections which have been
// disconnected in the background.
bool DBClientReplicaSet::isStillConnected() {
    if (_master && !_master->isStillConnected()) {
        resetMaster();
        // Don't notify monitor of bg failure, since it's not clear how long ago it happened
    }

    if (_lastSlaveOkConn.get() && !_lastSlaveOkConn->isStillConnected()) {
        resetSlaveOkConn();
        // Don't notify monitor of bg failure, since it's not clear how long ago it happened
    }

    return true;
}

namespace {

bool _isSecondaryCommand(StringData commandName, const BSONObj& commandArgs) {
    if (_secOkCmdList.count(commandName.toString())) {
        return true;
    }
    if (commandName == "mapReduce" || commandName == "mapreduce") {
        if (!commandArgs.hasField("out")) {
            return false;
        }

        BSONElement outElem(commandArgs["out"]);
        if (outElem.isABSONObj() && outElem["inline"].ok()) {
            return true;
        }
    }
    return false;
}

// Internal implementation of isSecondaryQuery, takes previously-parsed read preference
bool _isSecondaryQuery(const string& ns,
                       const BSONObj& queryObj,
                       const ReadPreferenceSetting& readPref) {
    // If the read pref is primary only, this is not a secondary query
    if (readPref.pref == ReadPreference::PrimaryOnly)
        return false;

    if (ns.find(".$cmd") == string::npos) {
        return true;
    }

    // This is a command with secondary-possible read pref
    // Only certain commands are supported for secondary operation.

    BSONObj actualQueryObj;
    if (strcmp(queryObj.firstElement().fieldName(), "query") == 0) {
        actualQueryObj = queryObj["query"].embeddedObject();
    } else {
        actualQueryObj = queryObj;
    }

    StringData commandName = actualQueryObj.firstElementFieldName();
    return _isSecondaryCommand(commandName, actualQueryObj);
}

}  // namespace


bool DBClientReplicaSet::isSecondaryQuery(const string& ns,
                                          const BSONObj& queryObj,
                                          int queryOptions) {
    unique_ptr<ReadPreferenceSetting> readPref(_extractReadPref(queryObj, queryOptions));
    return _isSecondaryQuery(ns, queryObj, *readPref);
}

DBClientConnection* DBClientReplicaSet::checkMaster() {
    ReplicaSetMonitorPtr monitor = _getMonitor();
    HostAndPort h = monitor->getMasterOrUassert();

    if (h == _masterHost && _master) {
        // a master is selected.  let's just make sure connection didn't die
        if (!_master->isFailed())
            return _master.get();

        monitor->failedHost(_masterHost);
        h = monitor->getMasterOrUassert();  // old master failed, try again.
    }

    _masterHost = h;

    ConnectionString connStr(_masterHost);

    string errmsg;
    DBClientConnection* newConn = NULL;

    try {
        // Needs to perform a dynamic_cast because we need to set the replSet
        // callback. We should eventually not need this after we remove the
        // callback.
        newConn = dynamic_cast<DBClientConnection*>(connStr.connect(errmsg, _so_timeout));
    } catch (const AssertionException& ex) {
        errmsg = ex.toString();
    }

    if (newConn == NULL || !errmsg.empty()) {
        monitor->failedHost(_masterHost);
        uasserted(ErrorCodes::FailedToSatisfyReadPreference,
                  str::stream() << "can't connect to new replica set master ["
                                << _masterHost.toString()
                                << "]"
                                << (errmsg.empty() ? "" : ", err: ")
                                << errmsg);
    }

    resetMaster();

    _masterHost = h;
    _master.reset(newConn);
    _master->setParentReplSetName(_setName);
    _master->setRequestMetadataWriter(getRequestMetadataWriter());
    _master->setReplyMetadataReader(getReplyMetadataReader());

    _authConnection(_master.get());
    return _master.get();
}

bool DBClientReplicaSet::checkLastHost(const ReadPreferenceSetting* readPref) {
    // Can't use a cached host if we don't have one.
    if (!_lastSlaveOkConn.get() || _lastSlaveOkHost.empty()) {
        return false;
    }

    // Don't pin if the readPrefs differ.
    if (!_lastReadPref || !_lastReadPref->equals(*readPref)) {
        return false;
    }

    // Make sure we don't think the host is down.
    if (_lastSlaveOkConn->isFailed() || !_getMonitor()->isHostUp(_lastSlaveOkHost)) {
        invalidateLastSlaveOkCache();
        return false;
    }

    return true;
}

void DBClientReplicaSet::_authConnection(DBClientConnection* conn) {
    for (map<string, BSONObj>::const_iterator i = _auths.begin(); i != _auths.end(); ++i) {
        try {
            conn->auth(i->second);
        } catch (const UserException&) {
            warning() << "cached auth failed for set: " << _setName
                      << " db: " << i->second[saslCommandUserDBFieldName].str()
                      << " user: " << i->second[saslCommandUserFieldName].str() << endl;
        }
    }
}

void DBClientReplicaSet::logoutAll(DBClientConnection* conn) {
    for (map<string, BSONObj>::const_iterator i = _auths.begin(); i != _auths.end(); ++i) {
        BSONObj response;
        try {
            conn->logout(i->first, response);
        } catch (const UserException& ex) {
            warning() << "Failed to logout: " << conn->getServerAddress() << " on db: " << i->first
                      << endl;
        }
    }
}

DBClientConnection& DBClientReplicaSet::masterConn() {
    return *checkMaster();
}

DBClientConnection& DBClientReplicaSet::slaveConn() {
    shared_ptr<ReadPreferenceSetting> readPref(
        new ReadPreferenceSetting(ReadPreference::SecondaryPreferred, TagSet()));
    DBClientConnection* conn = selectNodeUsingTags(readPref);

    uassert(16369,
            str::stream() << "No good nodes available for set: " << _getMonitor()->getName(),
            conn != NULL);

    return *conn;
}

bool DBClientReplicaSet::connect() {
    // Returns true if there are any up hosts.
    const ReadPreferenceSetting anyUpHost(ReadPreference::Nearest, TagSet());
    return _getMonitor()->getHostOrRefresh(anyUpHost).isOK();
}

static bool isAuthenticationException(const DBException& ex) {
    return ex.getCode() == ErrorCodes::AuthenticationFailed;
}

void DBClientReplicaSet::_auth(const BSONObj& params) {
    // We prefer to authenticate against a primary, but otherwise a secondary is ok too
    // Empty tag matches every secondary
    shared_ptr<ReadPreferenceSetting> readPref(
        new ReadPreferenceSetting(ReadPreference::PrimaryPreferred, TagSet()));

    LOG(3) << "dbclient_rs authentication of " << _getMonitor()->getName() << endl;

    // NOTE that we retry MAX_RETRY + 1 times, since we're always primary preferred we don't
    // fallback to the primary.
    Status lastNodeStatus = Status::OK();
    for (size_t retry = 0; retry < MAX_RETRY + 1; retry++) {
        try {
            DBClientConnection* conn = selectNodeUsingTags(readPref);

            if (conn == NULL) {
                break;
            }

            conn->auth(params);

            // Cache the new auth information since we now validated it's good
            _auths[params[saslCommandUserDBFieldName].str()] = params.getOwned();

            // Ensure the only child connection open is the one we authenticated against - other
            // child connections may not have full authentication information.
            // NOTE: _lastSlaveOkConn may or may not be the same as _master
            dassert(_lastSlaveOkConn.get() == conn || _master.get() == conn);
            if (conn != _lastSlaveOkConn.get()) {
                resetSlaveOkConn();
            }
            if (conn != _master.get()) {
                resetMaster();
            }

            return;
        } catch (const DBException& ex) {
            // We care if we can't authenticate (i.e. bad password) in credential params.
            if (isAuthenticationException(ex)) {
                throw;
            }

            StringBuilder errMsgB;
            errMsgB << "can't authenticate against replica set node "
                    << _lastSlaveOkHost.toString();
            lastNodeStatus = ex.toStatus(errMsgB.str());

            LOG(1) << lastNodeStatus.reason() << endl;
            invalidateLastSlaveOkCache();
        }
    }

    if (lastNodeStatus.isOK()) {
        StringBuilder assertMsgB;
        assertMsgB << "Failed to authenticate, no good nodes in " << _getMonitor()->getName();
        uasserted(ErrorCodes::NodeNotFound, assertMsgB.str());
    } else {
        uasserted(lastNodeStatus.code(), lastNodeStatus.reason());
    }
}

void DBClientReplicaSet::logout(const string& dbname, BSONObj& info) {
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
        } catch (const DBException&) {
            // Make sure we can't use this connection again.
            verify(_lastSlaveOkConn->isFailed());
        }
    }
}

// ------------- simple functions -----------------

void DBClientReplicaSet::insert(const string& ns, BSONObj obj, int flags) {
    checkMaster()->insert(ns, obj, flags);
}

void DBClientReplicaSet::insert(const string& ns, const vector<BSONObj>& v, int flags) {
    checkMaster()->insert(ns, v, flags);
}

void DBClientReplicaSet::remove(const string& ns, Query obj, int flags) {
    checkMaster()->remove(ns, obj, flags);
}

void DBClientReplicaSet::update(const string& ns, Query query, BSONObj obj, int flags) {
    return checkMaster()->update(ns, query, obj, flags);
}

unique_ptr<DBClientCursor> DBClientReplicaSet::query(const string& ns,
                                                     Query query,
                                                     int nToReturn,
                                                     int nToSkip,
                                                     const BSONObj* fieldsToReturn,
                                                     int queryOptions,
                                                     int batchSize) {
    shared_ptr<ReadPreferenceSetting> readPref(_extractReadPref(query.obj, queryOptions));
    if (_isSecondaryQuery(ns, query.obj, *readPref)) {
        LOG(3) << "dbclient_rs query using secondary or tagged node selection in "
               << _getMonitor()->getName() << ", read pref is " << readPref->toBSON()
               << " (primary : "
               << (_master.get() != NULL ? _master->getServerAddress() : "[not cached]")
               << ", lastTagged : " << (_lastSlaveOkConn.get() != NULL
                                            ? _lastSlaveOkConn->getServerAddress()
                                            : "[not cached]")
               << ")" << endl;

        string lastNodeErrMsg;
        for (size_t retry = 0; retry < MAX_RETRY; retry++) {
            try {
                DBClientConnection* conn = selectNodeUsingTags(readPref);

                if (conn == NULL) {
                    break;
                }

                unique_ptr<DBClientCursor> cursor = conn->query(
                    ns, query, nToReturn, nToSkip, fieldsToReturn, queryOptions, batchSize);

                return checkSlaveQueryResult(std::move(cursor));
            } catch (const DBException& dbExcep) {
                StringBuilder errMsgBuilder;
                errMsgBuilder << "can't query replica set node " << _lastSlaveOkHost.toString()
                              << ": " << causedBy(dbExcep);
                lastNodeErrMsg = errMsgBuilder.str();

                LOG(1) << lastNodeErrMsg << endl;
                invalidateLastSlaveOkCache();
            }
        }

        StringBuilder assertMsg;
        assertMsg << "Failed to do query, no good nodes in " << _getMonitor()->getName();
        if (!lastNodeErrMsg.empty()) {
            assertMsg << ", last error: " << lastNodeErrMsg;
        }

        uasserted(16370, assertMsg.str());
    }

    LOG(3) << "dbclient_rs query to primary node in " << _getMonitor()->getName() << endl;

    return checkMaster()->query(
        ns, query, nToReturn, nToSkip, fieldsToReturn, queryOptions, batchSize);
}

BSONObj DBClientReplicaSet::findOne(const string& ns,
                                    const Query& query,
                                    const BSONObj* fieldsToReturn,
                                    int queryOptions) {
    shared_ptr<ReadPreferenceSetting> readPref(_extractReadPref(query.obj, queryOptions));
    if (_isSecondaryQuery(ns, query.obj, *readPref)) {
        LOG(3) << "dbclient_rs findOne using secondary or tagged node selection in "
               << _getMonitor()->getName() << ", read pref is " << readPref->toBSON()
               << " (primary : "
               << (_master.get() != NULL ? _master->getServerAddress() : "[not cached]")
               << ", lastTagged : " << (_lastSlaveOkConn.get() != NULL
                                            ? _lastSlaveOkConn->getServerAddress()
                                            : "[not cached]")
               << ")" << endl;

        string lastNodeErrMsg;

        for (size_t retry = 0; retry < MAX_RETRY; retry++) {
            try {
                DBClientConnection* conn = selectNodeUsingTags(readPref);

                if (conn == NULL) {
                    break;
                }

                return conn->findOne(ns, query, fieldsToReturn, queryOptions);
            } catch (const DBException& dbExcep) {
                StringBuilder errMsgBuilder;
                errMsgBuilder << "can't findone replica set node " << _lastSlaveOkHost.toString()
                              << ": " << causedBy(dbExcep);
                lastNodeErrMsg = errMsgBuilder.str();

                LOG(1) << lastNodeErrMsg << endl;
                invalidateLastSlaveOkCache();
            }
        }

        StringBuilder assertMsg;
        assertMsg << "Failed to call findOne, no good nodes in " << _getMonitor()->getName();
        if (!lastNodeErrMsg.empty()) {
            assertMsg << ", last error: " << lastNodeErrMsg;
        }

        uasserted(16379, assertMsg.str());
    }

    LOG(3) << "dbclient_rs findOne to primary node in " << _getMonitor()->getName() << endl;

    return checkMaster()->findOne(ns, query, fieldsToReturn, queryOptions);
}

void DBClientReplicaSet::killCursor(long long cursorID) {
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
    ReplicaSetMonitorPtr monitor = ReplicaSetMonitor::get(_setName);
    if (monitor) {
        monitor->failedHost(_masterHost);
    }

    resetMaster();
}

unique_ptr<DBClientCursor> DBClientReplicaSet::checkSlaveQueryResult(
    unique_ptr<DBClientCursor> result) {
    if (result.get() == NULL)
        return result;

    BSONObj error;
    bool isError = result->peekError(&error);
    if (!isError)
        return result;

    // We only check for "not master or secondary" errors here

    // If the error code here ever changes, we need to change this code also
    BSONElement code = error["code"];
    if (code.isNumber() && code.Int() == ErrorCodes::NotMasterOrSecondary) {
        isntSecondary();
        throw DBException(str::stream() << "slave " << _lastSlaveOkHost.toString()
                                        << " is no longer secondary",
                          14812);
    }

    return result;
}

void DBClientReplicaSet::isntSecondary() {
    log() << "slave no longer has secondary status: " << _lastSlaveOkHost << endl;
    // Failover to next slave
    _getMonitor()->failedHost(_lastSlaveOkHost);

    resetSlaveOkConn();
}

DBClientConnection* DBClientReplicaSet::selectNodeUsingTags(
    shared_ptr<ReadPreferenceSetting> readPref) {
    if (checkLastHost(readPref.get())) {
        LOG(3) << "dbclient_rs selecting compatible last used node " << _lastSlaveOkHost;

        return _lastSlaveOkConn.get();
    }

    ReplicaSetMonitorPtr monitor = _getMonitor();

    auto selectedNodeStatus = monitor->getHostOrRefresh(*readPref);
    if (!selectedNodeStatus.isOK()) {
        LOG(3) << "dbclient_rs no compatible node found"
               << causedBy(selectedNodeStatus.getStatus());
        return nullptr;
    }

    const HostAndPort selectedNode = std::move(selectedNodeStatus.getValue());

    // We are now about to get a new connection from the pool, so cleanup
    // the current one and release it back to the pool.
    resetSlaveOkConn();

    _lastReadPref = readPref;
    _lastSlaveOkHost = selectedNode;

    // Primary connection is special because it is the only connection that is
    // versioned in mongos. Therefore, we have to make sure that this object
    // maintains only one connection to the primary and use that connection
    // every time we need to talk to the primary.
    if (monitor->isPrimary(selectedNode)) {
        checkMaster();

        LOG(3) << "dbclient_rs selecting primary node " << selectedNode << endl;

        _lastSlaveOkConn.reset(_master.get());

        return _master.get();
    }

    // Needs to perform a dynamic_cast because we need to set the replSet
    // callback. We should eventually not need this after we remove the
    // callback.
    DBClientConnection* newConn = dynamic_cast<DBClientConnection*>(
        globalConnPool.get(_lastSlaveOkHost.toString(), _so_timeout));

    // Assert here instead of returning NULL since the contract of this method is such
    // that returning NULL means none of the nodes were good, which is not the case here.
    uassert(16532,
            str::stream() << "Failed to connect to " << _lastSlaveOkHost.toString(),
            newConn != NULL);

    _lastSlaveOkConn.reset(newConn);
    _lastSlaveOkConn->setParentReplSetName(_setName);
    _lastSlaveOkConn->setRequestMetadataWriter(getRequestMetadataWriter());
    _lastSlaveOkConn->setReplyMetadataReader(getReplyMetadataReader());

    if (_authPooledSecondaryConn) {
        _authConnection(_lastSlaveOkConn.get());
    } else {
        // Mongos pooled connections are authenticated through
        // ShardingConnectionHook::onCreate().
    }

    LOG(3) << "dbclient_rs selecting node " << _lastSlaveOkHost << endl;

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

        shared_ptr<ReadPreferenceSetting> readPref(_extractReadPref(qm.query, qm.queryOptions));
        if (_isSecondaryQuery(qm.ns, qm.query, *readPref)) {
            LOG(3) << "dbclient_rs say using secondary or tagged node selection in "
                   << _getMonitor()->getName() << ", read pref is " << readPref->toBSON()
                   << " (primary : "
                   << (_master.get() != NULL ? _master->getServerAddress() : "[not cached]")
                   << ", lastTagged : " << (_lastSlaveOkConn.get() != NULL
                                                ? _lastSlaveOkConn->getServerAddress()
                                                : "[not cached]")
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
                } catch (const DBException& DBExcep) {
                    StringBuilder errMsgBuilder;
                    errMsgBuilder << "can't callLazy replica set node "
                                  << _lastSlaveOkHost.toString() << ": " << causedBy(DBExcep);
                    lastNodeErrMsg = errMsgBuilder.str();

                    LOG(1) << lastNodeErrMsg << endl;
                    invalidateLastSlaveOkCache();
                    continue;
                }

                return;
            }

            StringBuilder assertMsg;
            assertMsg << "Failed to call say, no good nodes in " << _getMonitor()->getName();
            if (!lastNodeErrMsg.empty()) {
                assertMsg << ", last error: " << lastNodeErrMsg;
            }

            uasserted(16380, assertMsg.str());
        }
    }

    LOG(3) << "dbclient_rs say to primary node in " << _getMonitor()->getName() << endl;

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

bool DBClientReplicaSet::recv(Message& m) {
    verify(_lazyState._lastClient);

    // TODO: It would be nice if we could easily wrap a conn error as a result error
    try {
        return _lazyState._lastClient->recv(m);
    } catch (DBException& e) {
        log() << "could not receive data from " << _lazyState._lastClient->toString() << causedBy(e)
              << endl;
        return false;
    }
}

void DBClientReplicaSet::checkResponse(const char* data,
                                       int nReturned,
                                       bool* retry,
                                       string* targetHost) {
    // For now, do exactly as we did before, so as not to break things.  In general though, we
    // should fix this so checkResponse has a more consistent contract.
    if (!retry) {
        if (_lazyState._lastClient)
            return _lazyState._lastClient->checkResponse(data, nReturned);
        else
            return checkMaster()->checkResponse(data, nReturned);
    }

    *retry = false;
    if (targetHost && _lazyState._lastClient)
        *targetHost = _lazyState._lastClient->getServerAddress();
    else if (targetHost)
        *targetHost = "";

    if (!_lazyState._lastClient)
        return;

    // nReturned == 1 means that we got one result back, which might be an error
    // nReturned == -1 is a sentinel value for "no data returned" aka (usually) network problem
    // If neither, this must be a query result so our response is ok wrt the replica set
    if (nReturned != 1 && nReturned != -1)
        return;

    BSONObj dataObj;
    if (nReturned == 1)
        dataObj = BSONObj(data);

    // Check if we should retry here
    if (_lazyState._lastOp == dbQuery && _lazyState._secondaryQueryOk) {
        // query could potentially go to a secondary, so see if this is an error (or empty) and
        // retry if we're not past our retry limit.

        if (nReturned == -1 /* no result, maybe network problem */ ||
            (hasErrField(dataObj) && !dataObj["code"].eoo() &&
             dataObj["code"].Int() == ErrorCodes::NotMasterOrSecondary)) {
            if (_lazyState._lastClient == _lastSlaveOkConn.get()) {
                isntSecondary();
            } else if (_lazyState._lastClient == _master.get()) {
                isntMaster();
            } else {
                warning() << "passed " << dataObj << " but last rs client "
                          << _lazyState._lastClient->toString() << " is not master or secondary"
                          << endl;
            }

            if (_lazyState._retries < static_cast<int>(MAX_RETRY)) {
                _lazyState._retries++;
                *retry = true;
            } else {
                log() << "too many retries (" << _lazyState._retries
                      << "), could not get data from replica set" << endl;
            }
        }
    } else if (_lazyState._lastOp == dbQuery) {
        // if query could not potentially go to a secondary, just mark the master as bad

        if (nReturned == -1 /* no result, maybe network problem */ ||
            (hasErrField(dataObj) && !dataObj["code"].eoo() &&
             dataObj["code"].Int() == ErrorCodes::NotMasterNoSlaveOk)) {
            if (_lazyState._lastClient == _master.get()) {
                isntMaster();
            }
        }
    }
}

rpc::UniqueReply DBClientReplicaSet::runCommandWithMetadata(StringData database,
                                                            StringData command,
                                                            const BSONObj& metadata,
                                                            const BSONObj& commandArgs) {
    // This overload exists so we can parse out the read preference and then use server
    // selection directly without having to re-parse the raw message.

    // TODO: eventually we will want to pass the metadata before serializing it to BSON
    // so we don't have to re-parse it, however, that will come with its own set of
    // complications (e.g. some kind of base class or concept for MetadataSerializable
    // objects). For now we do it the stupid way.
    auto ssm = uassertStatusOK(rpc::ServerSelectionMetadata::readFromMetadata(
        metadata.getField(rpc::ServerSelectionMetadata::fieldName())));

    // If we didn't get a readPref with this query, we assume SecondaryPreferred if secondaryOk
    // is true, and PrimaryOnly otherwise. This logic is replicated from _extractReadPref.
    auto defaultReadPref = ssm.isSecondaryOk()
        ? ReadPreferenceSetting(ReadPreference::SecondaryPreferred, TagSet())
        : ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly());

    auto readPref = ssm.getReadPreference().get_value_or(defaultReadPref);

    if (readPref.pref == ReadPreference::PrimaryOnly ||
        // If the command is not runnable on a secondary, we run it on the primary
        // regardless of the read preference.
        !_isSecondaryCommand(command, commandArgs)) {
        return checkMaster()->runCommandWithMetadata(
            std::move(database), std::move(command), metadata, commandArgs);
    }

    auto rpShared = std::make_shared<ReadPreferenceSetting>(std::move(readPref));

    for (size_t retry = 0; retry < MAX_RETRY; ++retry) {
        try {
            auto* conn = selectNodeUsingTags(rpShared);
            if (conn == nullptr) {
                break;
            }
            // We can't move database and command in case this throws
            // and we retry.
            return conn->runCommandWithMetadata(database, command, metadata, commandArgs);
        } catch (const DBException& ex) {
            log() << exceptionToStatus();
            invalidateLastSlaveOkCache();
        }
    }
    uasserted(ErrorCodes::NodeNotFound,
              str::stream() << "Could not satisfy $readPreference of '" << readPref.toBSON() << "' "
                            << "while attempting to run command "
                            << command);
}

bool DBClientReplicaSet::call(Message& toSend,
                              Message& response,
                              bool assertOk,
                              string* actualServer) {
    const char* ns = 0;

    if (toSend.operation() == dbQuery) {
        // TODO: might be possible to do this faster by changing api
        DbMessage dm(toSend);
        QueryMessage qm(dm);
        ns = qm.ns;

        shared_ptr<ReadPreferenceSetting> readPref(_extractReadPref(qm.query, qm.queryOptions));
        if (_isSecondaryQuery(ns, qm.query, *readPref)) {
            LOG(3) << "dbclient_rs call using secondary or tagged node selection in "
                   << _getMonitor()->getName() << ", read pref is " << readPref->toBSON()
                   << " (primary : "
                   << (_master.get() != NULL ? _master->getServerAddress() : "[not cached]")
                   << ", lastTagged : " << (_lastSlaveOkConn.get() != NULL
                                                ? _lastSlaveOkConn->getServerAddress()
                                                : "[not cached]")
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

                    return conn->call(toSend, response, assertOk, nullptr);
                } catch (const DBException& dbExcep) {
                    LOG(1) << "can't call replica set node " << _lastSlaveOkHost << ": "
                           << causedBy(dbExcep) << endl;

                    if (actualServer)
                        *actualServer = "";

                    invalidateLastSlaveOkCache();
                }
            }

            // Was not able to successfully send after max retries
            return false;
        }
    }

    LOG(3) << "dbclient_rs call to primary node in " << _getMonitor()->getName() << endl;

    DBClientConnection* m = checkMaster();
    if (actualServer)
        *actualServer = m->getServerAddress();

    if (!m->call(toSend, response, assertOk, nullptr))
        return false;

    if (ns) {
        QueryResult::View res = response.singleData().view2ptr();
        if (res.getNReturned() == 1) {
            BSONObj x(res.data());
            if (str::contains(ns, "$cmd")) {
                if (isNotMasterErrorString(x["errmsg"]))
                    isntMaster();
            } else {
                if (isNotMasterErrorString(getErrField(x)))
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
    _getMonitor()->failedHost(_lastSlaveOkHost);
    resetSlaveOkConn();
}

void DBClientReplicaSet::reset() {
    resetSlaveOkConn();
    _lazyState._lastClient = NULL;
    _lastReadPref.reset();
}

void DBClientReplicaSet::setAuthPooledSecondaryConn(bool setting) {
    _authPooledSecondaryConn = setting;
}

void DBClientReplicaSet::resetMaster() {
    if (_master.get() == _lastSlaveOkConn.get()) {
        _lastSlaveOkConn.release();
        _lastSlaveOkHost = HostAndPort();
    }

    _master.reset();
    _masterHost = HostAndPort();
}

void DBClientReplicaSet::resetSlaveOkConn() {
    if (_lastSlaveOkConn.get() == _master.get()) {
        _lastSlaveOkConn.release();
    } else if (_lastSlaveOkConn.get() != NULL) {
        if (_authPooledSecondaryConn) {
            logoutAll(_lastSlaveOkConn.get());
        } else {
            // Mongos pooled connections are all authenticated with the same credentials;
            // so no need to logout.
        }

        // If the connection was bad, the pool will clean it up.
        globalConnPool.release(_lastSlaveOkHost.toString(), _lastSlaveOkConn.release());
    }

    _lastSlaveOkHost = HostAndPort();
}

}  // namespace mongo
