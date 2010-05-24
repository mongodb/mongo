/**
*    Copyright (C) 2010 10gen Inc.
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

#include "pch.h"
#include "../commands.h"
#include "replset.h"
#include "multicmd.h"

namespace mongo { 

    class CmdReplSetElect : public Command {
    public:
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual bool logTheOp() { return false; }
        virtual LockType locktype() const { return NONE; }
        virtual void help( stringstream &help ) const { help << "internal"; }
        CmdReplSetElect() : Command("replSetElect") { }
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !replSet ) { 
                errmsg = "not running with --replSet";
                return false;
            }
            if( theReplSet == 0 ) {
                result.append("startupStatus", ReplSet::startupStatus);
                errmsg = ReplSet::startupStatusMsg.empty() ? "replset unknown error 2" : ReplSet::startupStatusMsg;
                return false;
            }
            theReplSet->elect.electCmdReceived(cmdObj, result);
            return true;
        }
    } cmdReplSetElect;

    int ReplSet::Consensus::totalVotes() const { 
        static int complain = 0;
        int vTot = rs._self->config().votes;
        for( Member *m = rs.head(); m; m=m->next() ) 
            vTot += m->config().votes;
        if( vTot % 2 == 0 && vTot && complain++ == 0 )
            log() << "replSet warning total number of votes is even - considering giving one member an extra vote" << rsLog;
        return vTot;
    }

    bool ReplSet::Consensus::aMajoritySeemsToBeUp() const {
        int vUp = rs._self->config().votes;
        for( Member *m = rs.head(); m; m=m->next() ) 
            vUp += m->hbinfo().up() ? m->config().votes : 0;
        return vUp * 2 > totalVotes();
    }

    static const int VETO = -10000;

    class VoteException : public std::exception {
    };

    const time_t LeaseTime = 30;

    unsigned ReplSet::Consensus::yea(unsigned memberId) /* throws VoteException */ {
        Atomic<LastYea>::tran t(ly);
        LastYea &ly = t.ref();
        time_t now = time(0);
        if( ly.when + LeaseTime >= now )
            throw VoteException();
        ly.when = now;
        ly.who = memberId;
        return rs._self->config().votes;
    }

    /* todo: threading **************** !!!!!!!!!!!!!!!! */
    void ReplSet::Consensus::electCmdReceived(BSONObj cmd, BSONObjBuilder& b) { 
        string name = cmd["name"].String();
        unsigned whoid = cmd["whoid"].Int();
        int cfgver = cmd["cfgver"].Int();
        OID round = cmd["round"].OID();
        int myver = rs.config().version;

        int vote = 0;
        if( name != rs.name() ) { 
            log() << "replSet error received an elect request for '" << name << "' but our set name is '" << rs.name() << "'" << rsLog;

        }
        else if( myver < cfgver ) { 
            // we are stale.  don't vote
        }
        else if( myver > cfgver ) { 
            // they are stale!
            log() << "replSet info got stale version # during election" << rsLog;
            vote = -10000;
        }
        else {
            try { 
                vote = yea(whoid);
            }
            catch(VoteException&) { 
                log() << "replSet voting no already voted for another" << rsLog;
            }
        }

        b.append("vote", vote);
        b.append("round", round);
    }

    void ReplSet::Consensus::_electSelf() {
        log() << "replSet info electSelf" << rsLog;
        time_t start = time(0);
        ReplSet::Member& me = *rs._self;        
        int tally = yea( me.id() );

        BSONObj electCmd = BSON(
               "replSetElect" << 1 <<
               "set" << rs.name() << 
               "who" << me.fullName() << 
               "whoid" << me.hbinfo().id() << 
               "cfgver" << rs._cfg->version << 
               "round" << OID::gen() /* this is just for diagnostics */
            );

        list<Target> L;
        for( Member *m = rs.head(); m; m=m->next() )
            if( m->hbinfo().up() )
                L.push_back( Target(m->fullName()) );

        multiCommand(electCmd, L);

        for( list<Target>::iterator i = L.begin(); i != L.end(); i++ ) {
            cout << "TEMP elect res: " << i->result.toString() << endl;
            if( i->ok ) {
                int v = i->result["vote"].Int();
                tally += v;
            }
        }
        if( tally*2 > totalVotes() ) {
            if( time(0) - start > 30 ) {
                // defensive; should never happen as we have timeouts on connection and operation for our conn
                log() << "replSet too much time passed during election, ignoring result" << rsLog;
            }
            /* succeeded. */
            rs._myState = PRIMARY;
            log() << "replSet elected self as primary" << rsLog;
            return;
        } 
        else { 
            log() << "replSet couldn't elect self, majority not achieved tally:" << tally << rsLog;
        }
    }

    void ReplSet::Consensus::electSelf() {
        try { 
            _electSelf(); 
        } 
        catch(VoteException& ) { 
            log() << "replSet not trying to elect self as responded yea to someone else recently" << rsLog;
        }
        catch(...) { }
    }

}
