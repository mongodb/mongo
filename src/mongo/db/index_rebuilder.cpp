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

#include "mongo/db/instance.h"
#include "mongo/db/pdfile.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

    IndexRebuilder indexRebuilder;

    IndexRebuilder::IndexRebuilder() {}

    std::string IndexRebuilder::name() const {
        return "IndexRebuilder";
    }

    /**
     * This resets memory tracking to its original value after all indexes are rebuilt.
     *
     * Before the server starts listening, all memory accesses are counted as taking 0 time (because
     * the timer hasn't started yet).  The Record class warns about these 0-time accesses (actually,
     * the warning is in Rolling, which is used by Record) so run() turns off the tracking to
     * silence these warnings.  We want to make sure that they're turned back on, though, no matter
     * how run() exits.
     */
    static void resetMemoryTracking(bool originalTracking) {
        Record::MemoryTrackingEnabled = originalTracking;
    }

    void IndexRebuilder::run() {
        // Disable record access timer warnings
        ON_BLOCK_EXIT(resetMemoryTracking, Record::MemoryTrackingEnabled);
        Record::MemoryTrackingEnabled = false;

        Client::initThread(name().c_str()); 
        Client::GodScope gs;

        bool firstTime = true;
        std::vector<std::string> dbNames;
        getDatabaseNames(dbNames);

        for (std::vector<std::string>::const_iterator it = dbNames.begin();
             it < dbNames.end();
             it++) {
            checkDB(*it, &firstTime);
        }

        cc().shutdown();
    }

    void IndexRebuilder::checkDB(const std::string& dbName, bool* firstTime) {
        const std::string systemNS = dbName + ".system.namespaces";
        DBDirectClient cli;
        scoped_ptr<DBClientCursor> cursor(cli.query(systemNS, Query()));

        // This depends on system.namespaces not changing while we iterate
        while (cursor->more()) {
            BSONObj nsDoc = cursor->next();
            const char* ns = nsDoc["name"].valuestrsafe();

            // This write lock is held throughout the index building process
            // for this namespace.
            Client::WriteContext ctx(ns);
            NamespaceDetails* nsd = nsdetails(ns);

            if (!nsd || !nsd->indexBuildsInProgress) {
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
                int inProg = nsd->indexBuildsInProgress;

                getDur().writingInt(nsd->indexBuildsInProgress) = 0;

                for (int i = 0; i < inProg; i++) {
                    nsd->idx(nsd->nIndexes+i).kill_idx();
                }

                continue;
            }

            // We go from right to left building these indexes, so that indexBuildInProgress-- has
            // the correct effect of "popping" an index off the list.
            while (nsd->indexBuildsInProgress > 0) {
                retryIndexBuild(dbName, nsd, nsd->nIndexes+nsd->indexBuildsInProgress-1);
            }
        }
    }

    void IndexRebuilder::retryIndexBuild(const std::string& dbName,
                                         NamespaceDetails* nsd,
                                         const int index) {
        // details.info is always a valid system.indexes entry because DataFileMgr::insert journals
        // creating the index doc and then insert_makeIndex durably assigns its DiskLoc to info.
        // indexBuildsInProgress is set after that, so if it is set, info must be set.
        IndexDetails& details = nsd->idx(index);

        // First, clean up the in progress index build.  Save the system.indexes entry so that we
        // can add it again afterwards.
        BSONObj indexObj = details.info.obj().getOwned();

        // Clean up the in-progress index build
        getDur().writingInt(nsd->indexBuildsInProgress) -= 1;
        details.kill_idx();

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
