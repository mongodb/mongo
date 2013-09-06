/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/s/cluster_client_internal.h"

#include <string>
#include <vector>

#include "mongo/client/connpool.h"
#include "mongo/db/field_parser.h"
#include "mongo/s/type_changelog.h"
#include "mongo/s/type_mongos.h"
#include "mongo/s/type_shard.h"
#include "mongo/util/stringutils.h"

namespace mongo {

    using std::string;
    using std::vector;
    using mongoutils::str::stream;

    Status checkClusterMongoVersions(const ConnectionString& configLoc,
                                     const string& minMongoVersion)
    {
        scoped_ptr<ScopedDbConnection> connPtr;

        //
        // Find mongos pings in config server
        //

        try {
            connPtr.reset(new ScopedDbConnection(configLoc, 30));
            ScopedDbConnection& conn = *connPtr;
            scoped_ptr<DBClientCursor> cursor(_safeCursor(conn->query(MongosType::ConfigNS,
                                                                      Query())));

            while (cursor->more()) {

                BSONObj pingDoc = cursor->next();

                MongosType ping;
                string errMsg;
                // NOTE: We don't care if the ping is invalid, legacy stuff will be
                if (!ping.parseBSON(pingDoc, &errMsg)) {
                    warning() << "could not parse ping document: " << pingDoc << causedBy(errMsg)
                              << endl;
                    continue;
                }

                string mongoVersion = "2.0";
                // Hack to determine older mongos versions from ping format
                if (ping.isWaitingSet()) mongoVersion = "2.2";
                if (ping.isMongoVersionSet() && ping.getMongoVersion() != "") {
                    mongoVersion = ping.getMongoVersion();
                }

                Date_t lastPing = ping.getPing();

                long long quietIntervalMillis = 0;
                Date_t currentJsTime = jsTime();
                if (currentJsTime >= lastPing) {
                    quietIntervalMillis = static_cast<long long>(currentJsTime - lastPing);
                }
                long long quietIntervalMins = quietIntervalMillis / (60 * 1000);

                // We assume that anything that hasn't pinged in 5 minutes is probably down
                if (quietIntervalMins >= 5) {
                    log() << "stale mongos detected " << quietIntervalMins << " minutes ago,"
                          << " network location is " << pingDoc["_id"].String()
                          << ", not checking version" << endl;
            	}
                else {
                    if (versionCmp(mongoVersion, minMongoVersion) < 0) {
                        return Status(ErrorCodes::RemoteValidationError,
                                      stream() << "version " << mongoVersion << " of mongos at "
                                               << ping.getName()
                                               << " is not compatible with the config update, "
                                               << "you must wait 5 minutes "
                                               << "after shutting down a pre-" << minMongoVersion
                                               << " mongos");
                    }
                }
            }
        }
        catch (const DBException& e) {
            return e.toStatus("could not read mongos pings collection");
        }

        //
        // Load shards from config server
        //

        vector<ConnectionString> shardLocs;

        try {
            ScopedDbConnection& conn = *connPtr;
            scoped_ptr<DBClientCursor> cursor(_safeCursor(conn->query(ShardType::ConfigNS,
                                                                      Query())));

            while (cursor->more()) {

                BSONObj shardDoc = cursor->next();

                ShardType shard;
                string errMsg;
                if (!shard.parseBSON(shardDoc, &errMsg) || !shard.isValid(&errMsg)) {
                    connPtr->done();
                    return Status(ErrorCodes::UnsupportedFormat,
                                  stream() << "invalid shard " << shardDoc
                                           << " read from the config server" << causedBy(errMsg));
                }

                ConnectionString shardLoc = ConnectionString::parse(shard.getHost(), errMsg);
                if (shardLoc.type() == ConnectionString::INVALID) {
                    connPtr->done();
                    return Status(ErrorCodes::UnsupportedFormat,
                                  stream() << "invalid shard host " << shard.getHost()
                                           << " read from the config server" << causedBy(errMsg));
                }

                shardLocs.push_back(shardLoc);
            }
        }
        catch (const DBException& e) {
            return e.toStatus("could not read shards collection");
        }

        connPtr->done();

        //
        // We've now got all the shard info from the config server, start contacting the shards
        // and verifying their versions.
        //

        for (vector<ConnectionString>::iterator it = shardLocs.begin(); it != shardLocs.end(); ++it)
        {
            ConnectionString& shardLoc = *it;

            vector<HostAndPort> servers = shardLoc.getServers();

            for (vector<HostAndPort>::iterator serverIt = servers.begin();
                    serverIt != servers.end(); ++serverIt)
            {
                // Note: This will *always* be a single-host connection
                ConnectionString serverLoc(*serverIt);

                log() << "checking that version of host " << serverLoc << " is compatible with " 
                      << minMongoVersion << endl;

                scoped_ptr<ScopedDbConnection> serverConnPtr;

                bool resultOk;
                BSONObj serverStatus;

                try {
                    serverConnPtr.reset(new ScopedDbConnection(serverLoc, 30));
                    ScopedDbConnection& serverConn = *serverConnPtr;

                    resultOk = serverConn->runCommand("admin",
                                                      BSON("serverStatus" << 1),
                                                      serverStatus);
                }
                catch (const DBException& e) {
                    warning() << "could not run server status command on " << serverLoc.toString()
                              << causedBy(e) << ", you must manually verify this mongo server is "
                              << "offline (for at least 5 minutes) or of a version >= 2.2" << endl;
                    continue;
                }

                // TODO: Make running commands saner such that we can consolidate error handling
                if (!resultOk) {
                    return Status(ErrorCodes::UnknownError,
                                  stream() << DBClientConnection::getLastErrorString(serverStatus)
                                           << causedBy(serverStatus.toString()));
                }

                serverConnPtr->done();

                verify(serverStatus["version"].type() == String);
                string mongoVersion = serverStatus["version"].String();

                if (versionCmp(mongoVersion, minMongoVersion) < 0) {
                    return Status(ErrorCodes::RemoteValidationError,
                                  stream() << "version " << mongoVersion << " of mongo server at "
                                           << serverLoc.toString()
                                           << " is not compatible with the config update");
                }
            }
        }

        return Status::OK();
    }

    Status _findAllCollections(const ConnectionString& configLoc,
                               bool optionalEpochs,
                               OwnedPointerMap<string, CollectionType>* collections)
    {
        scoped_ptr<ScopedDbConnection> connPtr;

        try {
            connPtr.reset(new ScopedDbConnection(configLoc, 30));

            ScopedDbConnection& conn = *connPtr;
            scoped_ptr<DBClientCursor> cursor(_safeCursor(conn->query(CollectionType::ConfigNS,
                                                                      Query())));

            while (cursor->more()) {

                BSONObj collDoc = cursor->nextSafe();

                // Replace with unique_ptr (also owned ptr map goes away)
                auto_ptr<CollectionType> coll(new CollectionType());
                string errMsg;
                bool parseOk = coll->parseBSON(collDoc, &errMsg);

                // Needed for the v3 to v4 upgrade
                bool epochNotSet = !coll->isEpochSet() || !coll->getEpoch().isSet();
                if (optionalEpochs && epochNotSet) {
                    // Set our epoch to something here, just to allow
                    coll->setEpoch(OID::gen());
                }

                if (!parseOk || !coll->isValid(&errMsg)) {
                    return Status(ErrorCodes::UnsupportedFormat,
                                  stream() << "invalid collection " << collDoc
                                           << " read from the config server" << causedBy(errMsg));
                }

                if (coll->isDroppedSet() && coll->getDropped()) {
                    continue;
                }

                if (optionalEpochs && epochNotSet) {
                    coll->setEpoch(OID());
                }
    
                // Get NS before releasing
                string ns = coll->getNS();
                collections->mutableMap().insert(make_pair(ns, coll.release()));
            }
        }
        catch (const DBException& e) {
            return e.toStatus();
        }

        connPtr->done();
        return Status::OK();
    }

    Status findAllCollections(const ConnectionString& configLoc,
                              OwnedPointerMap<string, CollectionType>* collections)
    {
        return _findAllCollections(configLoc, false, collections);
    }

    Status findAllCollectionsV3(const ConnectionString& configLoc,
                                OwnedPointerMap<string, CollectionType>* collections)
    {
        return _findAllCollections(configLoc, true, collections);
    }

    Status findAllChunks(const ConnectionString& configLoc,
                         const string& ns,
                         OwnedPointerVector<ChunkType>* chunks)
    {
        scoped_ptr<ScopedDbConnection> connPtr;
        scoped_ptr<DBClientCursor> cursor;

        try {
            connPtr.reset(new ScopedDbConnection(configLoc, 30));
            ScopedDbConnection& conn = *connPtr;
            scoped_ptr<DBClientCursor> cursor(_safeCursor(conn->query(ChunkType::ConfigNS,
                                                                      BSON(ChunkType::ns(ns)))));

            while (cursor->more()) {

                BSONObj chunkDoc = cursor->nextSafe();

                // TODO: replace with unique_ptr when available
                auto_ptr<ChunkType> chunk(new ChunkType());
                string errMsg;
                if (!chunk->parseBSON(chunkDoc, &errMsg) || !chunk->isValid(&errMsg)) {
                    connPtr->done();
                    return Status(ErrorCodes::UnsupportedFormat,
                                  stream() << "invalid chunk " << chunkDoc
                                           << " read from the config server" << causedBy(errMsg));
                }

                chunks->mutableVector().push_back(chunk.release());
            }
        }
        catch (const DBException& e) {
            return e.toStatus();
        }

        connPtr->done();
        return Status::OK();
    }

    Status logConfigChange(const ConnectionString& configLoc,
                           const string& clientHost,
                           const string& ns,
                           const string& description,
                           const BSONObj& details)
    {
        //
        // The code for writing to the changelog collection exists elsewhere - we duplicate here to
        // avoid dependency issues.
        // TODO: Merge again once config.cpp is cleaned up.
        //

        string changeID = stream() << getHostNameCached() << "-" << terseCurrentTime() << "-"
                                   << OID::gen();

        ChangelogType changelog;
        changelog.setChangeID(changeID);
        changelog.setServer(getHostNameCached());
        changelog.setClientAddr(clientHost == "" ? "N/A" : clientHost);
        changelog.setTime(jsTime());
        changelog.setWhat(description);
        changelog.setNS(ns);
        changelog.setDetails(details);

        log() << "about to log new metadata event: " << changelog.toBSON() << endl;

        scoped_ptr<ScopedDbConnection> connPtr;

        try {
            connPtr.reset(new ScopedDbConnection(configLoc, 30));
            ScopedDbConnection& conn = *connPtr;

            // TODO: better way here
            static bool createdCapped = false;
            if (!createdCapped) {

                try {
                    conn->createCollection(ChangelogType::ConfigNS, 1024 * 1024 * 10, true);
                }
                catch (const DBException& e) {
                    // don't care, someone else may have done this for us
                    // if there's still a problem, caught in outer try
                    LOG(1) << "couldn't create the changelog, continuing " << e << endl;
                }

                createdCapped = true;
            }

            conn->insert(ChangelogType::ConfigNS, changelog.toBSON());
            _checkGLE(conn);
        }
        catch (const DBException& e) {
            // if we got here, it means the config change is only in the log,
            // it didn't make it to config.changelog
            log() << "not logging config change: " << changeID << causedBy(e) << endl;
            return e.toStatus();
        }

        connPtr->done();
        return Status::OK();
    }

    // Helper function for safe writes to non-SCC config servers
    void _checkGLE(ScopedDbConnection& conn) {
        string error = conn->getLastError();
        if (error != "") {
            conn.done();
            // TODO: Make error handling more consistent, throwing and re-catching makes things much
            // simpler to manage
            uasserted(16624, str::stream() << "operation failed" << causedBy(error));
        }
    }

    // Helper function for safe cursors
    DBClientCursor* _safeCursor(auto_ptr<DBClientCursor> cursor) {
        // TODO: Make error handling more consistent, it's annoying that cursors error out by
        // throwing exceptions *and* being empty
        uassert(16625, str::stream() << "cursor not found, transport error", cursor.get());
        return cursor.release();
    }

}
