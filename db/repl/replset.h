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

    class ReplSet;
    extern ReplSet *theReplSet;

    struct RemoteServer { 
        RemoteServer() : _port(-1) { }
        RemoteServer(string h, int p = -1) : _host(h), _port(p) { 
        }
        string _host;
        int _port;
        bool operator<(const RemoteServer& r) const { return _host < r._host || (_host==r._host&&_port<r._port); }
    };

    /* information about the entire repl set, such as the various servers in the set, and their state */
    class ReplSet {
    public:
        string name;

        /* cfgString format is 
           replsetname/host1,host2:port,...
           where :port is optional, and 
           */
        ReplSet(string cfgString);

    private:
        string _name;
        vector<RemoteServer> _seeds;
        vector<RemoteServer> _members;
        static void healthThread();
        void health();
    };

}
