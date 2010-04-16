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

namespace mongo {

    class ReplSet;
    extern ReplSet *theReplSet;

    struct RemoteServer { 
        RemoteServer() : _port(-1) { }
        RemoteServer(string h, int p = -1) : _host(h), _port(p) { }
        bool operator<(const RemoteServer& r) const { return _host < r._host || (_host==r._host&&_port<r._port); }
    private:
        // invariant (except full obj assignment):
        string _host;
        int _port;
    };

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
        const vector<RemoteServer> *_seeds;

        struct MemberInfo : public List1<MemberInfo>::Base {
            const char *host;
            int port;
        };
        List1<MemberInfo> _members;

        void f() { 
            MemberInfo* m = _members.head();
            if( m ) 
                m->next();
            _members.orphan(m);
        }

        void startHealth();
    };

}
