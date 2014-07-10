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

    bool anyReplEnabled();

    /* replication slave? (possibly with slave)
       --slave cmd line setting -> SimpleSlave
    */
    typedef enum { NotSlave=0, SimpleSlave } SlaveTypes;

    class ReplSettings {
    public:
        SlaveTypes slave;

        /** true means we are master and doing replication.  if we are not writing to oplog, this won't be true. */
        bool master;

        bool fastsync;

        bool autoresync;

        int slavedelay;

        long long oplogSize;   // --oplogSize

        // for master/slave replication
        std::string source;    // --source
        std::string only;      // --only
        int pretouch;          // --pretouch for replication application (experimental)

        std::string replSet;       // --replSet[/<seedlist>]
        std::string ourSetName() const {
            std::string setname;
            size_t sl = replSet.find('/');
            if( sl == std::string::npos )
                return replSet;
            return replSet.substr(0, sl);
        }
        bool usingReplSets() const { return !replSet.empty(); }

        std::string rsIndexPrefetch;// --indexPrefetch

        std::set<std::string> discoveredSeeds;
        mutex discoveredSeeds_mx;

        BSONObj reconfig;

        ReplSettings()
            : slave(NotSlave),
            master(false),
            fastsync(),
            autoresync(false),
            slavedelay(),
            oplogSize(0),
            pretouch(0),
            discoveredSeeds(),
            discoveredSeeds_mx("ReplSettings::discoveredSeeds") {
        }

        // TODO(spencer): Remove explicit copy constructor after we no longer have mutable state
        // in ReplSettings.
        ReplSettings(const ReplSettings& other) :
            slave(other.slave),
            master(other.master),
            fastsync(other.fastsync),
            autoresync(other.autoresync),
            slavedelay(other.slavedelay),
            oplogSize(other.oplogSize),
            source(other.source),
            only(other.only),
            pretouch(other.pretouch),
            replSet(other.replSet),
            rsIndexPrefetch(other.rsIndexPrefetch),
            discoveredSeeds(other.discoveredSeeds),
            discoveredSeeds_mx("ReplSettings::discoveredSeeds"),
            reconfig(other.reconfig.getOwned()) {}

        ReplSettings& operator=(const ReplSettings& other) {
            if (this == &other) return *this;

            slave = other.slave;
            master = other.master;
            fastsync = other.fastsync;
            autoresync = other.autoresync;
            slavedelay = other.slavedelay;
            oplogSize = other.oplogSize;
            source = other.source;
            only = other.only;
            pretouch = other.pretouch;
            replSet = other.replSet;
            rsIndexPrefetch = other.rsIndexPrefetch;
            discoveredSeeds = other.discoveredSeeds;
            reconfig = other.reconfig.getOwned();
            return *this;
        }

    };

} // namespace repl
} // namespace mongo
