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
#include "../../util/concurrency/value.h"
#include "../../util/hostandport.h"

namespace mongo {

    extern class ReplSet *theReplSet;

    /* information about the entire repl set, such as the various servers in the set, and their state */
    /* note: We currently do not free mem when the set goes away - it is assumed the replset is a 
             singleton and long lived.
    */
    class ReplSet {
    public:
        bool fatal;

        bool ok() const { return !fatal; }

        /* @return replica set's logical name */
        string getName() const { return _name; }

        /* cfgString format is 
           replsetname/host1,host2:port,...
           where :port is optional.

           throws exception if a problem initializing.
        */
        ReplSet(string cfgString);

        // for replSetGetStatus command
        void summarizeStatus(BSONObjBuilder&) const;

    private:
        string _name;
        const vector<HostAndPort> *_seeds;

        /** load our configuration from admin.replset.  try seed machines too. 
            throws exception if a problem.
        */
        void loadConfig();

//        void addMemberIfMissing(const HostAndPort& p);

        struct MemberInfo : public List1<MemberInfo>::Base {
            MemberInfo(string h, int p) : port(p), host(h) {
                dead = false;
                lastHeartbeat = 0;
                upSince = 0;
                health = -1.0;
            }

            bool dead;
            const int port;
            const string host;
            double health;
            time_t lastHeartbeat;
            time_t upSince;
            DiagStr lastHeartbeatErrMsg;

            string fullName() {
                if( port < 0 ) return host;
                stringstream ss;
                ss << host << ':' << port;
                return ss.str();
            }
        };
        /* all members of the set EXCEPT SELF. */
        List1<MemberInfo> _members;

        void startHealthThreads();
        friend class FeedbackThread;
    };

}
