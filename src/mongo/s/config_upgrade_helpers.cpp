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
 */

#include "mongo/s/config_upgrade_helpers.h"

#include "mongo/client/connpool.h"
#include "mongo/db/namespacestring.h"
#include "mongo/s/cluster_client_internal.h"
#include "mongo/util/timer.h"

namespace mongo {

    using mongoutils::str::stream;

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

    string _extractHashFor(const BSONObj& dbHashResult, const string& collName) {

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
            resultOk = conn->runCommand(nssA.db, BSON("dbHash" << true), result);
            conn.done();
        }
        catch (const DBException& e) {
            return e.toStatus();
        }

        if (!resultOk) {
            return Status(ErrorCodes::UnknownError,
                          stream() << "could not run dbHash command on " << nssA.db << " db"
                                   << causedBy(result.toString()));
        }

        string hashResultA = _extractHashFor(result, nssA.coll);

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
            resultOk = conn->runCommand(nssB.db, BSON("dbHash" << true), result);
            conn.done();
        }
        catch (const DBException& e) {
            return e.toStatus();
        }

        if (!resultOk) {
            return Status(ErrorCodes::UnknownError,
                          stream() << "could not run dbHash command on " << nssB.db << " db"
                                   << causedBy(result.toString()));
        }

        string hashResultB = _extractHashFor(result, nssB.coll);

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

    Status copyFrozenCollection(const ConnectionString& configLoc,
                                const string& fromNS,
                                const string& toNS)
    {
        scoped_ptr<ScopedDbConnection> connPtr;
        auto_ptr<DBClientCursor> cursor;

        // Create new collection
        bool resultOk;
        BSONObj createResult;

        try {
            connPtr.reset(new ScopedDbConnection(configLoc, 30));
            ScopedDbConnection& conn = *connPtr;

            resultOk = conn->createCollection(toNS, 0, false, 0, &createResult);
        }
        catch (const DBException& e) {
            return e.toStatus("could not create new collection");
        }

        if (!resultOk) {
            return Status(ErrorCodes::UnknownError,
                          stream() << DBClientWithCommands::getLastErrorString(createResult)
                                   << causedBy(createResult.toString()));
        }

        NamespaceString fromNSS(fromNS);
        NamespaceString toNSS(toNS);

        // Copy indexes over
        try {
            ScopedDbConnection& conn = *connPtr;

            verify(fromNSS.isValid());

            // TODO: EnsureIndex at some point, if it becomes easier?
            string indexesNS = fromNSS.db + ".system.indexes";
            scoped_ptr<DBClientCursor> cursor(_safeCursor(conn->query(indexesNS,
                                                                      BSON("ns" << fromNS))));

            while (cursor->more()) {

                BSONObj next = cursor->nextSafe();

                BSONObjBuilder newIndexDesc;
                newIndexDesc.append("ns", toNS);
                newIndexDesc.appendElementsUnique(next);

                conn->insert(toNSS.db + ".system.indexes", newIndexDesc.done());
                _checkGLE(conn);
            }
        }
        catch (const DBException& e) {
            return e.toStatus("could not create indexes in new collection");
        }

        //
        // Copy data over in batches. A batch size here is way smaller than the maximum size of
        // a bsonobj. We want to copy efficiently but we don't need to maximize the object size
        // here.
        //

        Timer t;
        int64_t docCount = 0;
        const int32_t maxBatchSize = BSONObjMaxUserSize / 16;
        try {
            log() << "About to copy " << fromNS << " to " << toNS << endl;

            ScopedDbConnection& conn = *connPtr;
            scoped_ptr<DBClientCursor> cursor(_safeCursor(conn->query(fromNS, BSONObj())));

            vector<BSONObj> insertBatch;
            int32_t insertSize = 0;
            while (cursor->more()) {

                BSONObj next = cursor->nextSafe().getOwned();
                ++docCount;

                insertBatch.push_back(next);
                insertSize += next.objsize();

                if (insertSize > maxBatchSize ) {
                    conn->insert(toNS, insertBatch);
                    _checkGLE(conn);
                    insertBatch.clear();
                    insertSize = 0;
                }

                if (t.seconds() >= 10) {
                    t.reset();
                    log() << "Copied " << docCount << " documents so far from "
                          << fromNS << " to " << toNS << endl;
                }
            }

            if (!insertBatch.empty()) {
                conn->insert(toNS, insertBatch);
                _checkGLE(conn);
            }

            log() << "Finished copying " << docCount << " documents from "
                  << fromNS << " to " << toNS << endl;

        }
        catch (const DBException& e) {
            return e.toStatus("could not copy data into new collection");
        }

        connPtr->done();

        // Verify indices haven't changed
        Status indexStatus = checkIdsTheSame(configLoc,
                                             fromNSS.db + ".system.indexes",
                                             toNSS.db + ".system.indexes");

        if (!indexStatus.isOK()) {
            return indexStatus;
        }

        // Verify data hasn't changed
        return checkHashesTheSame(configLoc, fromNS, toNS);
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
}
