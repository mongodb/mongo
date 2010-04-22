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

    extern bool replSet; // true if using repl sets
    extern class ReplSet *theReplSet; // null until initialized

    /* information about the entire repl set, such as the various servers in the set, and their state */
    /* note: We currently do not free mem when the set goes away - it is assumed the replset is a 
             singleton and long lived.
    */
    class ReplSet {
    public:
        bool isMaster(const char *client) { 
//
            return false;
        }
        void fillIsMaster(BSONObjBuilder&);

        static enum StartupStatus { PRESTART, LOADINGCONFIG, BADCONFIG, EMPTYCONFIG, FINISHME } startupStatus;
        static string startupStatusMsg;
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
            MemberInfo(string h, int p) : _port(p), _host(h) {
                _dead = false;
                _lastHeartbeat = 0;
                _upSince = 0;
                _health = -1.0;
            }
            string fullName() const {
                if( _port < 0 ) return _host;
                stringstream ss;
                ss << _host << ':' << _port;
                return ss.str();
            }
            double health() const { return _health; }
            time_t upSince() const { return _upSince; }
            time_t lastHeartbeat() const { return _lastHeartbeat; }
        private:
            friend class FeedbackThread; // feedbackthread is the primary writer to these objects

            bool _dead;
            const int _port;
            const string _host;
            double _health;
            time_t _lastHeartbeat;
            time_t _upSince;
        public:
            DiagStr _lastHeartbeatErrMsg;
        };
        /* all members of the set EXCEPT SELF. */
        List1<MemberInfo> _members;

        void startHealthThreads();
        friend class FeedbackThread;
    };

}
