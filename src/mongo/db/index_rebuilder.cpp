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

#include "mongo/db/index_rebuilder.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/instance.h"
#include "mongo/db/pdfile.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

    IndexRebuilder indexRebuilder;

    IndexRebuilder::IndexRebuilder() {}

    std::string IndexRebuilder::name() const {
        return "IndexRebuilder";
    }

    void IndexRebuilder::run() {
        Client::initThread(name().c_str()); 
        ON_BLOCK_EXIT_OBJ(cc(), &Client::shutdown);
        cc().getAuthorizationSession()->grantInternalAuthorization(
                UserName("_indexRebuilder", "local"));

        bool firstTime = true;
        std::vector<std::string> dbNames;
        getDatabaseNames(dbNames);

        try {
            for (std::vector<std::string>::const_iterator it = dbNames.begin();
                 it < dbNames.end();
                 it++) {
                checkDB(*it, &firstTime);
            }
        }
        catch (const DBException&) {
            warning() << "index rebuilding did not complete" << endl;
        }
        LOG(1) << "checking complete" << endl;
    }

    void IndexRebuilder::checkDB(const std::string& dbName, bool* firstTime) {
        const std::string systemNS = dbName + ".system.namespaces";
        DBDirectClient cli;
        scoped_ptr<DBClientCursor> cursor(cli.query(systemNS, Query()));

        // This depends on system.namespaces not changing while we iterate
        while (cursor->more()) {
            BSONObj nsDoc = cursor->nextSafe();
            const char* ns = nsDoc["name"].valuestrsafe();
            LOG(1) << "checking ns " << ns << " for interrupted index builds" << endl;
            // This write lock is held throughout the index building process
            // for this namespace.
            Client::WriteContext ctx(ns);
            NamespaceDetails* nsd = nsdetails(ns);

            if ( nsd == NULL || nsd->getIndexBuildsInProgress() == 0 ) {
                continue;
            }

            log() << "found interrupted index build(s) on " << ns << endl;
            if (*firstTime) {
                log() << "note: restart the server with --noIndexBuildRetry to skip index rebuilds"
                      << endl;
                *firstTime = false;
            }

            // If the indexBuildRetry flag isn't set, just clear the inProg flag
            if (!cmdLine.indexBuildRetry) {
                // If we crash between unsetting the inProg flag and cleaning up the index, the
                // index space will be lost.
                nsd->blowAwayInProgressIndexEntries();
                continue;
            }

            // We go from right to left building these indexes, so that indexBuildInProgress-- has
            // the correct effect of "popping" an index off the list.
            while ( nsd->getTotalIndexCount() > nsd->getCompletedIndexCount() ) {
                retryIndexBuild(dbName, nsd);
            }
        }
    }

    void IndexRebuilder::retryIndexBuild(const std::string& dbName,
                                         NamespaceDetails* nsd ) {
        // First, clean up the in progress index build.  Save the system.indexes entry so that we
        // can add it again afterwards.
        BSONObj indexObj = nsd->prepOneUnfinishedIndex();

        // The index has now been removed from system.indexes, so the only record of it is in-
        // memory. If there is a journal commit between now and when insert() rewrites the entry and
        // the db crashes before the new system.indexes entry is journalled, the index will be lost
        // forever.  Thus, we're assuming no journaling will happen between now and the entry being
        // re-written.

        try {
            const std::string ns = dbName + ".system.indexes";
            theDataFileMgr.insert(ns.c_str(), indexObj.objdata(), indexObj.objsize(), false, true);
        }
        catch (const DBException& e) {
            log() << "building index failed: " << e.what() << " (" << e.getCode() << ")"
                  << endl;
        }
    }
}
