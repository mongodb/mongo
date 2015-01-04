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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/s/config_upgrade_helpers.h"

#include <boost/scoped_ptr.hpp>

#include "mongo/client/connpool.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/cluster_client_internal.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/type_config_version.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

    using boost::scoped_ptr;
    using mongoutils::str::stream;

    // Custom field used in upgrade state to determine if/where we failed on last upgrade
    const BSONField<bool> inCriticalSectionField("inCriticalSection", false);

    Status checkIdsTheSame(const ConnectionString& configLoc, const string& nsA, const string& nsB)
    {
        scoped_ptr<ScopedDbConnection> connPtr;
        auto_ptr<DBClientCursor> cursor;

        try {
            connPtr.reset(new ScopedDbConnection(configLoc, 30));
            ScopedDbConnection& conn = *connPtr;

            scoped_ptr<DBClientCursor> cursorA(_safeCursor(conn->query(nsA,
                                                                       Query().sort(BSON("_id" << 1)))));
            scoped_ptr<DBClientCursor> cursorB(_safeCursor(conn->query(nsB,
                                                                       Query().sort(BSON("_id" << 1)))));

            while (cursorA->more() && cursorB->more()) {

                BSONObj nextA = cursorA->nextSafe();
                BSONObj nextB = cursorB->nextSafe();

                if (nextA["_id"] != nextB["_id"]) {
                    connPtr->done();

                    return Status(ErrorCodes::RemoteValidationError,
                                  stream() << "document " << nextA << " is not the same as "
                                           << nextB);
                }
            }

            if (cursorA->more() != cursorB->more()) {
                connPtr->done();

                return Status(ErrorCodes::RemoteValidationError,
                              stream() << "collection " << (cursorA->more() ? nsA : nsB)
                                       << " has more documents than "
                                       << (cursorA->more() ? nsB : nsA));
            }
        }
        catch (const DBException& e) {
            return e.toStatus();
        }

        connPtr->done();
        return Status::OK();
    }

    string _extractHashFor(const BSONObj& dbHashResult, const StringData& collName) {

        if (dbHashResult["collections"].type() != Object
            || dbHashResult["collections"].Obj()[collName].type() != String)
        {
            return "";
        }

        return dbHashResult["collections"].Obj()[collName].String();
    }

    Status checkHashesTheSame(const ConnectionString& configLoc,
                              const string& nsA,
                              const string& nsB)
    {
        //
        // Check the sizes first, b/c if one collection is empty the hash check will fail
        //

        unsigned long long countA;
        unsigned long long countB;

        try {
            ScopedDbConnection conn(configLoc, 30);
            countA = conn->count(nsA, BSONObj());
            countB = conn->count(nsB, BSONObj());
            conn.done();
        }
        catch (const DBException& e) {
            return e.toStatus();
        }

        if (countA == 0 && countB == 0) {
            return Status::OK();
        }
        else if (countA != countB) {
            return Status(ErrorCodes::RemoteValidationError,
                          stream() << "collection " << nsA << " has " << countA << " documents but "
                                   << nsB << " has " << countB << "documents");
        }
        verify(countA == countB);

        //
        // Find hash for nsA
        //

        bool resultOk;
        BSONObj result;

        NamespaceString nssA(nsA);

        try {
            ScopedDbConnection conn(configLoc, 30);
            resultOk = conn->runCommand(nssA.db().toString(), BSON("dbHash" << true), result);
            conn.done();
        }
        catch (const DBException& e) {
            return e.toStatus();
        }

        if (!resultOk) {
            return Status(ErrorCodes::UnknownError,
                          stream() << "could not run dbHash command on " << nssA.db() << " db"
                                   << causedBy(result.toString()));
        }

        string hashResultA = _extractHashFor(result, nssA.coll());

        if (hashResultA == "") {
            return Status(ErrorCodes::RemoteValidationError,
                          stream() << "could not find hash for collection " << nsA << " in "
                                   << result.toString());
        }

        //
        // Find hash for nsB
        //

        NamespaceString nssB(nsB);

        try {
            ScopedDbConnection conn(configLoc, 30);
            resultOk = conn->runCommand(nssB.db().toString(), BSON("dbHash" << true), result);
            conn.done();
        }
        catch (const DBException& e) {
            return e.toStatus();
        }

        if (!resultOk) {
            return Status(ErrorCodes::UnknownError,
                          stream() << "could not run dbHash command on " << nssB.db() << " db"
                                   << causedBy(result.toString()));
        }

        string hashResultB = _extractHashFor(result, nssB.coll());

        if (hashResultB == "") {
            return Status(ErrorCodes::RemoteValidationError,
                          stream() << "could not find hash for collection " << nsB << " in "
                                   << result.toString());
        }

        if (hashResultA != hashResultB) {
            return Status(ErrorCodes::RemoteValidationError,
                          stream() << "collection hashes for collection " << nsA << " and " << nsB
                                   << " do not match");
        }

        return Status::OK();
    }

    Status overwriteCollection(const ConnectionString& configLoc,
                               const string& fromNS,
                               const string& overwriteNS)
    {

        // TODO: Also a bit awkward to deal with command results
        bool resultOk;
        BSONObj renameResult;

        // Create new collection
        try {
            ScopedDbConnection conn(configLoc, 30);

            BSONObjBuilder bob;
            bob.append("renameCollection", fromNS);
            bob.append("to", overwriteNS);
            bob.append("dropTarget", true);
            BSONObj renameCommand = bob.obj();

            resultOk = conn->runCommand("admin", renameCommand, renameResult);
            conn.done();
        }
        catch (const DBException& e) {
            return e.toStatus();
        }

        if (!resultOk) {
            return Status(ErrorCodes::UnknownError,
                          stream() << DBClientWithCommands::getLastErrorString(renameResult)
                                   << causedBy(renameResult.toString()));
        }

        return Status::OK();
    }

    string genWorkingSuffix(const OID& lastUpgradeId) {
        return "-upgrade-" + lastUpgradeId.toString();
    }

    string genBackupSuffix(const OID& lastUpgradeId) {
        return "-backup-" + lastUpgradeId.toString();
    }

    Status preUpgradeCheck(const ConnectionString& configServer,
                           const VersionType& lastVersionInfo,
                           string minMongosVersion) {
        if (lastVersionInfo.isUpgradeIdSet() && lastVersionInfo.getUpgradeId().isSet()) {
            //
            // Another upgrade failed, so cleanup may be necessary
            //

            BSONObj lastUpgradeState = lastVersionInfo.getUpgradeState();

            bool inCriticalSection;
            string errMsg;
            if (!FieldParser::extract(lastUpgradeState,
                                      inCriticalSectionField,
                                      &inCriticalSection,
                                      &errMsg)) {
                return Status(ErrorCodes::FailedToParse, causedBy(errMsg));
            }

            if (inCriticalSection) {
                // Note: custom message must be supplied by caller
                return Status(ErrorCodes::ManualInterventionRequired, "");
            }
        }

        //
        // Check the versions of other mongo processes in the cluster before upgrade.
        // We can't upgrade if there are active pre-v2.4 processes in the cluster
        //
        return checkClusterMongoVersions(configServer, string(minMongosVersion));
    }

    Status startConfigUpgrade(const string& configServer,
                              int currentVersion,
                              const OID& upgradeID) {
        BSONObjBuilder setUpgradeIdObj;
        setUpgradeIdObj << VersionType::upgradeId(upgradeID);
        setUpgradeIdObj << VersionType::upgradeState(BSONObj());

        Status result = clusterUpdate(VersionType::ConfigNS,
                BSON("_id" << 1 << VersionType::currentVersion(currentVersion)),
                BSON("$set" << setUpgradeIdObj.done()),
                false, // upsert
                false, // multi
                WriteConcernOptions::AllConfigs,
                NULL);

        if ( !result.isOK() ) {
            return Status( result.code(),
                           str::stream() << "could not initialize version info"
                                         << "for upgrade: " << result.reason() );
        }
        return result;
    }

    Status enterConfigUpgradeCriticalSection(const string& configServer, int currentVersion) {
        BSONObjBuilder setUpgradeStateObj;
        setUpgradeStateObj.append(VersionType::upgradeState(), BSON(inCriticalSectionField(true)));

        Status result = clusterUpdate(VersionType::ConfigNS,
                BSON("_id" << 1 << VersionType::currentVersion(currentVersion)),
                BSON("$set" << setUpgradeStateObj.done()),
                false, // upsert
                false, // multi
                WriteConcernOptions::AllConfigs,
                NULL);

        log() << "entered critical section for config upgrade" << endl;

        // No cleanup message here since we're not sure if we wrote or not, and
        // not dangerous either way except to prevent further updates (at which point
        // the message is printed)

        if ( !result.isOK() ) {
            return Status( result.code(), str::stream() << "could not update version info"
                                                        << "to enter critical update section: "
                                                        << result.reason() );
        }

        return result;
    }


    Status commitConfigUpgrade(const string& configServer,
                               int currentVersion,
                               int minCompatibleVersion,
                               int newVersion) {

        // Note: DO NOT CLEAR the config version unless bumping the minCompatibleVersion,
        // we want to save the excludes that were set.

        BSONObjBuilder setObj;
        setObj << VersionType::minCompatibleVersion(minCompatibleVersion);
        setObj << VersionType::currentVersion(newVersion);

        BSONObjBuilder unsetObj;
        unsetObj.append(VersionType::upgradeId(), 1);
        unsetObj.append(VersionType::upgradeState(), 1);
        unsetObj.append("version", 1); // remove deprecated field, no longer supported >= v2.8.

        Status result = clusterUpdate(VersionType::ConfigNS,
                BSON("_id" << 1 << VersionType::currentVersion(currentVersion)),
                BSON("$set" << setObj.done() << "$unset" << unsetObj.done()),
                false, // upsert
                false, // multi,
                WriteConcernOptions::AllConfigs,
                NULL);

        if ( !result.isOK() ) {
            return Status( result.code(), str::stream() << "could not write new version info "
                                                        << " and exit critical upgrade section: "
                                                        << result.reason() );
        }

        return result;
    }

}
