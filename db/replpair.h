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
#include "dbhelpers.h"
#include "json.h"
#include "../client/dbclient.h"
#include "repl.h"
#include "cmdline.h"
#include "repl/rs.h"

namespace mongo {

    extern const char *replAllDead;

    /* ReplPair is a pair of db servers replicating to one another and cooperating.

       Only one member of the pair is active at a time; so this is a smart master/slave
       configuration basically.

       You may read from the slave at anytime though (if you don't mind the slight lag).

       todo: Could be extended to be more than a pair, thus the name 'Set' -- for example,
       a set of 3...
    */

    class ReplPair {
    public:
        enum ReplState {
            State_CantArb = -3,
            State_Confused = -2,
            State_Negotiating = -1,
            State_Slave = 0,
            State_Master = 1
        };

        int state;
        ThreadSafeString info; // commentary about our current state
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
        virtual ~ReplPair() {}

        bool dominant(const string& myname) {
            if ( myname == remoteHost )
                return cmdLine.port > remotePort;
            return myname > remoteHost;
        }

        void setMasterLocked( int n, const char *_comment = "" ) {
            dblock p;
            setMaster( n, _comment );
        }

        void setMaster(int n, const char *_comment = "");

        /* negotiate with our peer who is master; returns state of peer */
        int negotiate(DBClientConnection *conn, string method);

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

       If 'client' is not specified, the current client is used.
    */
    inline bool _isMaster() {
        if( replSet ) {
            if( theReplSet ) 
                return theReplSet->isPrimary();
            return false;
        }

        if( ! replSettings.slave ) 
            return true;

        if ( replAllDead )
            return false;

        if ( replPair ) {
            if( replPair->state == ReplPair::State_Master )
                return true;
        }
        else { 
            if( replSettings.master ) {
                // if running with --master --slave, allow.  note that master is also true 
                // for repl pairs so the check for replPair above is important.
                return true;
            }
        }
        
        if ( cc().isGod() )
            return true;
        
        return false;
    }
    inline bool isMaster(const char *client = 0) {
        if( _isMaster() )
            return true;
        if ( !client ) {
            Database *database = cc().database();
            assert( database );
            client = database->name.c_str();
        }
        return strcmp( client, "local" ) == 0;
    }

    inline void notMasterUnless(bool expr) { 
        uassert( 10107 , "not master" , expr );
    }

    /* we allow queries to SimpleSlave's -- but not to the slave (nonmaster) member of a replica pair 
       so that queries to a pair are realtime consistent as much as possible.  use setSlaveOk() to 
       query the nonmaster member of a replica pair.
    */
    inline void replVerifyReadsOk(ParsedQuery& pq) {
        if( replSet ) {
            /* todo: speed up the secondary case.  as written here there are 2 mutex entries, it can b 1. */
            if( isMaster() ) return;
            uassert(13435, "not master and slaveok=false", pq.hasOption(QueryOption_SlaveOk));
            uassert(13436, "not master or secondary, can't read", theReplSet && theReplSet->isSecondary() );
        } else {
            notMasterUnless(isMaster() || pq.hasOption(QueryOption_SlaveOk) || replSettings.slave == SimpleSlave );
        }
    }

    inline bool isMasterNs( const char *ns ) {
        char cl[ 256 ];
        nsToDatabase( ns, cl );
        return isMaster( cl );
    }

    inline ReplPair::ReplPair(const char *remoteEnd, const char *arb) {
        state = -1;
        remote = remoteEnd;
        remotePort = CmdLine::DefaultDBPort;
        remoteHost = remoteEnd;
        const char *p = strchr(remoteEnd, ':');
        if ( p ) {
            remoteHost = string(remoteEnd, p-remoteEnd);
            remotePort = atoi(p+1);
            uassert( 10125 , "bad port #", remotePort > 0 && remotePort < 0x10000 );
            if ( remotePort == CmdLine::DefaultDBPort )
                remote = remoteHost; // don't include ":27017" as it is default; in case ran in diff ways over time to normalizke the hostname format in sources collection
        }

        uassert( 10126 , "arbiter parm is missing, use '-' for none", arb);
        arbHost = arb;
        uassert( 10127 , "arbiter parm is empty", !arbHost.empty());
    }

    /* This is set to true if we have EVER been up to date -- this way a new pair member
     which is a replacement won't go online as master until we have initially fully synced.
     */
    class PairSync {
        int initialsynccomplete;
    public:
        PairSync() {
            initialsynccomplete = -1;
        }

        /* call before using the class.  from dbmutex */
        void init() {
            BSONObj o;
            initialsynccomplete = 0;
            if ( Helpers::getSingleton("local.pair.sync", o) )
                initialsynccomplete = 1;
        }

        bool initialSyncCompleted() {
            return initialsynccomplete != 0;
        }

        void setInitialSyncCompleted() {
            BSONObj o = fromjson("{\"initialsynccomplete\":1}");
            Helpers::putSingleton("local.pair.sync", o);
            initialsynccomplete = 1;
            tlog() << "pair: initial sync complete" << endl;
        }

        void setInitialSyncCompletedLocking() {
            if ( initialsynccomplete == 1 )
                return;
            dblock lk;
            setInitialSyncCompleted();
        }
    };


} // namespace mongo
