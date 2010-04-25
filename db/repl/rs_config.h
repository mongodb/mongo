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
     [, heartbeatConnRetries  : <n>]
     [, getLastErrorDefaults: <defaults>]
   }
 }
*/

class ReplSetConfig {
public:
    /* if something is misconfigured, throws an exception. 
       if couldn't be queried or is just blank, ok() will be false.
       */
    ReplSetConfig(const HostAndPort& h);

    ReplSetConfig(BSONObj cfg);

    bool ok() const { return _ok; }

    struct Member {
        Member() : _id(-1), votes(1), priority(1.0), arbiterOnly(false) { }
        int _id;              /* ordinal */
        HostAndPort h;
        double priority;      /* 0 means can never be primary */
        bool arbiterOnly;
        unsigned votes;       /* how many votes this node gets. default 1. */
        void check() const;   /* check validity, assert if not. */
    };
    vector<Member> members;
    string _id;
    int version;
    HealthOptions ho;
    string md5;

    // true if could connect, and there is no cfg object there at all
    bool empty() { return version == -2; }

    /* TODO: add getLastErrorDefaults */

private:
    bool _ok;
    void from(BSONObj);
    void clear();
    BSONObj asBson() const;
};

}
