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

#pragma once

#include <set>
#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/util/concurrency/mutex.h"


namespace mongo {
namespace repl {

extern int maxSyncSourceLagSecs;
extern double replElectionTimeoutOffsetLimitFraction;

bool anyReplEnabled();

/* replication slave? (possibly with slave)
   --slave cmd line setting -> SimpleSlave
*/
typedef enum { NotSlave = 0, SimpleSlave } SlaveTypes;

class ReplSettings {
public:
    std::string ourSetName() const {
        std::string setname;
        size_t sl = replSet.find('/');
        if (sl == std::string::npos)
            return replSet;
        return replSet.substr(0, sl);
    }
    bool usingReplSets() const {
        return !replSet.empty();
    }

    SlaveTypes slave = NotSlave;

    /**
     * true means we are master and doing replication.  if we are not writing to oplog, this won't
     * be true.
     */
    bool master = false;

    bool fastsync = false;

    bool autoresync = false;

    int slavedelay = 0;

    long long oplogSize = 0;  // --oplogSize

    /**
     * True means that the majorityReadConcern feature is enabled, either explicitly by the user or
     * implicitly by a requiring feature such as CSRS. It does not mean that the storage engine
     * supports snapshots or that the snapshot thread is running. Those are tracked separately.
     */
    bool majorityReadConcernEnabled = false;

    // for master/slave replication
    std::string source;  // --source
    std::string only;    // --only
    int pretouch = 0;    // --pretouch for replication application (experimental)

    std::string replSet;  // --replSet[/<seedlist>]

    std::string rsIndexPrefetch;  // --indexPrefetch
};

}  // namespace repl
}  // namespace mongo
