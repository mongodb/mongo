// rs_config.h
// repl set configuration
//

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

#include "../../util/hostandport.h"
#include "health.h"

namespace mongo { 

/* 
http://www.mongodb.org/display/DOCS/Replica+Set+Internals#ReplicaSetInternals-Configuration

admin.replset

This collection has one object per server in the set.  The objects have the form:

 { set : <logical_set_name>, host : <hostname[+port]>
   [, priority: <priority>]
   [, arbiterOnly : true]
 }

Additionally an object in this collection holds global configuration settings for the set:

 { _id : <logical_set_name>, settings:
   { [heartbeatSleep : <seconds>]
     [, heartbeatTimeout : <seconds>]
     [, connRetries : <n>]
     [, getLastErrorDefaults: <defaults>]
   }
 }
*/

class ReplSetConfig {
public:
    /* can throw exception */
    ReplSetConfig(const HostAndPort& h);

    struct Member {
        HostAndPort h;
        double priority;  /* 0 means can never be primary */
        bool arbiterOnly;
    };
    vector<Member> members;
    string _id;
    int version;
    HealthOptions healthOptions;
    string md5;

    /* TODO: add getLastErrorDefaults */

private:
    void from(BSONObj);
};

}