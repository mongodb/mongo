// /db/repl/replset.h

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

#include "../../util/concurrency/list.h"
#include "../../util/hostandport.h"

namespace mongo {

    extern class ReplSet *theReplSet;

    /* information about the entire repl set, such as the various servers in the set, and their state */
    /* note: We currently do not free mem when the set goes away - it is assumed the replset is a 
             singleton and long lived.
    */
    class ReplSet {
    public:
        string getName() const { return _name; }

        /* cfgString format is 
           replsetname/host1,host2:port,...
           where :port is optional, and 
           */
        ReplSet(string cfgString);

    private:
        string _name;
        const vector<HostAndPort> *_seeds;

        struct MemberInfo : public List1<MemberInfo>::Base {
            MemberInfo() : dead(false), lastHeartbeat(0) { }
            bool dead;
            const char *host;
            int port;

            time_t lastHeartbeat;
        };
        /* all members of the set EXCEPT SELF. */
        List1<MemberInfo> _members;

        void startHealthThreads();
        friend class FeedbackThread;
    };

}
