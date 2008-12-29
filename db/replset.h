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

#include "db.h"
#include "../client/dbclient.h"

extern int port;
extern const char *allDead;

/* ReplPair is a pair of db servers replicating to one another and cooperating.

   Only one member of the pair is active at a time; so this is a smart master/slave
   configuration basically.

   You may read from the slave at anytime though (if you don't mind the slight lag).

   todo: Could be extended to be more than a pair, thus the name 'Set' -- for example,
   a set of 3...
*/

class ReplPair {
public:
    enum {
        State_CantArb = -3,
        State_Confused = -2,
        State_Negotiating = -1,
        State_Slave = 0,
        State_Master = 1
    };

    int state;
    string info; // commentary about our current state
    string arbHost;  // "-" for no arbiter.  "host[:port]"
    int remotePort;
    string remoteHost;
    string remote; // host:port if port specified.
//    int date; // -1 not yet set; 0=slave; 1=master

    string getInfo() {
        stringstream ss;
        ss << "  state:   ";
        if ( state == 1 ) ss << "1 State_Master ";
        else if ( state == 0 ) ss << "0 State_Slave";
        else
            ss << "<b>" << state << "</b>";
        ss << '\n';
        ss << "  info:    " << info << '\n';
        ss << "  arbhost: " << arbHost << '\n';
        ss << "  remote:  " << remoteHost << ':' << remotePort << '\n';
//        ss << "  date:    " << date << '\n';
        return ss.str();
    }

    ReplPair(const char *remoteEnd, const char *arbiter);

    bool dominant(const string& myname) {
        if ( myname == remoteHost )
            return port > remotePort;
        return myname > remoteHost;
    }

    void setMasterLocked( int n, const char *_comment = "" ) {
        dblock p;
        setMaster( n, _comment );
    }

    void setMaster(int n, const char *_comment = "");

    /* negotiate with our peer who is master */
    void negotiate(DBClientConnection *conn);

    /* peer unreachable, try our arbitrator */
    void arbitrate();

    virtual
    DBClientConnection *newClientConnection() const {
        return new DBClientConnection();
    }
};

extern ReplPair *replPair;

/* note we always return true for the "local" namespace.

   we should not allow most operations when not the master
   also we report not master if we are "dead".

   See also CmdIsMaster.

*/
inline bool isMaster() {
    if ( allDead ) {
        return database->name == "local";
    }

    if ( replPair == 0 || replPair->state == ReplPair::State_Master )
        return true;

    return database->name == "local";
}

inline ReplPair::ReplPair(const char *remoteEnd, const char *arb) {
    state = -1;
    remote = remoteEnd;
    remotePort = DBPort;
    remoteHost = remoteEnd;
    const char *p = strchr(remoteEnd, ':');
    if ( p ) {
        remoteHost = string(remoteEnd, p-remoteEnd);
        remotePort = atoi(p+1);
        uassert("bad port #", remotePort > 0 && remotePort < 0x10000 );
        if ( remotePort == DBPort )
            remote = remoteHost; // don't include ":27017" as it is default; in case ran in diff ways over time to normalizke the hostname format in sources collection
    }

    uassert("arbiter parm is missing, use '-' for none", arb);
    arbHost = arb;
    uassert("arbiter parm is empty", !arbHost.empty());
}
