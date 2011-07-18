// replset.h

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

namespace mongo {

    /* throws */
    bool requestHeartbeat(string setname, string fromHost, string memberFullName, BSONObj& result, int myConfigVersion, int& theirConfigVersion, bool checkEmpty = false);

    struct HealthOptions {
        HealthOptions() :  
            heartbeatSleepMillis(2000), 
            heartbeatTimeoutMillis( 10000 ),
            heartbeatConnRetries(2) 
        { }

        bool isDefault() const { return *this == HealthOptions(); }

        // see http://www.mongodb.org/display/DOCS/Replica+Set+Internals
        unsigned heartbeatSleepMillis;
        unsigned heartbeatTimeoutMillis;
        unsigned heartbeatConnRetries ;

        void check() {
            uassert(13112, "bad replset heartbeat option", heartbeatSleepMillis >= 10);
            uassert(13113, "bad replset heartbeat option", heartbeatTimeoutMillis >= 10);
        }

        bool operator==(const HealthOptions& r) const {
            return heartbeatSleepMillis==r.heartbeatSleepMillis && heartbeatTimeoutMillis==r.heartbeatTimeoutMillis && heartbeatConnRetries==r.heartbeatConnRetries;
        }
    };
    
}
