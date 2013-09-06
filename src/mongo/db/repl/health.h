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

#include "mongo/db/jsobj.h"

namespace mongo {

    /* throws */
    bool requestHeartbeat(const std::string& setname,
                          const std::string& fromHost,
                          const std::string& memberFullName,
                          BSONObj& result,
                          int myConfigVersion,
                          int& theirConfigVersion,
                          bool checkEmpty = false);

    struct HealthOptions {
        HealthOptions() :  
            heartbeatSleepMillis(2000), 
            heartbeatTimeoutMillis( 10000 ),
            heartbeatConnRetries(2) 
        { }

        bool isDefault() const { return *this == HealthOptions(); }

        // see http://dochub.mongodb.org/core/replicasetinternals
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
