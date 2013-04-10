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
*/

#pragma once

#include <set>
#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/util/concurrency/mutex.h"


namespace mongo {

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

        std::set<std::string> discoveredSeeds;
        mutex discoveredSeeds_mx;

        BSONObj reconfig;

        ReplSettings()
            : slave(NotSlave),
            master(false),
            fastsync(),
            autoresync(false),
            slavedelay(),
            discoveredSeeds(),
            discoveredSeeds_mx("ReplSettings::discoveredSeeds") {
        }

    };

    extern ReplSettings replSettings;
}
