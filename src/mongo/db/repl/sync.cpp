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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/db/repl/sync.h"

#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/db/client.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

    void Sync::setHostname(const string& hostname) {
        hn = hostname;
    }

    BSONObj Sync::getMissingDoc(const BSONObj& o) {
        OplogReader missingObjReader; // why are we using OplogReader to run a non-oplog query?
        const char *ns = o.getStringField("ns");

        // capped collections
        NamespaceDetails *nsd = nsdetails(ns);
        if ( nsd && nsd->isCapped() ) {
            log() << "replication missing doc, but this is okay for a capped collection (" << ns << ")" << endl;
            return BSONObj();
        }

        const int retryMax = 3;
        for (int retryCount = 1; retryCount <= retryMax; ++retryCount) {
            if (retryCount != 1) {
                // if we are retrying, sleep a bit to let the network possibly recover
                sleepsecs(retryCount * retryCount);
            }
            try {
                bool ok = missingObjReader.connect(hn);
                if (!ok) {
                    warning() << "network problem detected while connecting to the "
                              << "sync source, attempt " << retryCount << " of "
                              << retryMax << endl;
                        continue;  // try again
                }
            } 
            catch (const SocketException&) {
                warning() << "network problem detected while connecting to the "
                          << "sync source, attempt " << retryCount << " of "
                          << retryMax << endl;
                continue; // try again
            }

            // might be more than just _id in the update criteria
            BSONObj query = BSONObjBuilder().append(o.getObjectField("o2")["_id"]).obj();
            BSONObj missingObj;
            try {
                missingObj = missingObjReader.findOne(ns, query);
            } 
            catch (const SocketException&) {
                warning() << "network problem detected while fetching a missing document from the "
                          << "sync source, attempt " << retryCount << " of "
                          << retryMax << endl;
                continue; // try again
            } 
            catch (DBException& e) {
                log() << "replication assertion fetching missing object: " << e.what() << endl;
                throw;
            }

            // success!
            return missingObj;
        }
        // retry count exceeded
        msgasserted(15916, 
                    str::stream() << "Can no longer connect to initial sync source: " << hn);
    }

    bool Sync::shouldRetry(const BSONObj& o) {
        // should already have write lock
        const char *ns = o.getStringField("ns");
        Client::Context ctx(ns);

        // we don't have the object yet, which is possible on initial sync.  get it.
        log() << "replication info adding missing object" << endl; // rare enough we can log

        BSONObj missingObj = getMissingDoc(o);

        if( missingObj.isEmpty() ) {
            log() << "replication missing object not found on source. presumably deleted later in oplog" << endl;
            log() << "replication o2: " << o.getObjectField("o2").toString() << endl;
            log() << "replication o firstfield: " << o.getObjectField("o").firstElementFieldName() << endl;

            return false;
        }
        else {
            DiskLoc d = theDataFileMgr.insert(ns, (void*) missingObj.objdata(), missingObj.objsize());
            uassert(15917, "Got bad disk location when attempting to insert", !d.isNull());

            LOG(1) << "replication inserted missing doc: " << missingObj.toString() << endl;
            return true;
        }
    }
}
