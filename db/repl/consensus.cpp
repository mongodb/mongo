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
#include "rs.h"
#include "multicmd.h"

namespace mongo { 

    class CmdReplSetFresh : public ReplSetCommand { 
    public:
        CmdReplSetFresh() : ReplSetCommand("replSetFresh") { }
    private:
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) )
                return false;
            errmsg = "not done";
            return false;
        }
    } cmdReplSetFresh;

    class CmdReplSetElect : public ReplSetCommand {
    public:
        CmdReplSetElect() : ReplSetCommand("replSetElect") { }
    private:
        virtual bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) )
                return false;
            //task::lam f = boost::bind(&Consensus::electCmdReceived, &theReplSet->elect, cmdObj, &result);
            //theReplSet->mgr->call(f);
            theReplSet->elect.electCmdReceived(cmdObj, &result);
            return true;
        }
    } cmdReplSetElect;

    int Consensus::totalVotes() const { 
        static int complain = 0;
        int vTot = rs._self->config().votes;
        for( Member *m = rs.head(); m; m=m->next() ) 
            vTot += m->config().votes;
        if( vTot % 2 == 0 && vTot && complain++ == 0 )
            log() << "replSet warning total number of votes is even - considering giving one member an extra vote" << rsLog;
        return vTot;
    }

    bool Consensus::aMajoritySeemsToBeUp() const {
        int vUp = rs._self->config().votes;
        for( Member *m = rs.head(); m; m=m->next() ) 
            vUp += m->hbinfo().up() ? m->config().votes : 0;
        return vUp * 2 > totalVotes();
    }

    static const int VETO = -10000;

    class VoteException : public std::exception {
    };

    const time_t LeaseTime = 30;

    unsigned Consensus::yea(unsigned memberId) /* throws VoteException */ {
        Atomic<LastYea>::tran t(ly);
        LastYea &ly = t.ref();
        time_t now = time(0);
        if( ly.when + LeaseTime >= now && ly.who != memberId ) {
            log() << "replSet TEMP not voting yea for " << memberId << rsLog;
            log() << "replSet TEMP voted for " << ly.who << ' ' << now-ly.when << " secs ago" << rsLog;
            throw VoteException();
        }
        ly.when = now;
        ly.who = memberId;
        return rs._self->config().votes;
    }

    /* todo: threading **************** !!!!!!!!!!!!!!!! */
    void Consensus::electCmdReceived(BSONObj cmd, BSONObjBuilder* _b) { 
        BSONObjBuilder& b = *_b;
        log() << "replSet TEMP RECEIVED ELECT MSG " << cmd.toString() << rsLog;
        string set = cmd["set"].String();
        unsigned whoid = cmd["whoid"].Int();
        int cfgver = cmd["cfgver"].Int();
        OID round = cmd["round"].OID();
        int myver = rs.config().version;

        int vote = 0;
        if( set != rs.name() ) { 
            log() << "replSet error received an elect request for '" << set << "' but our set name is '" << rs.name() << "'" << rsLog;

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
                rs.relinquish();
                log() << "replSet info voting yea for " << whoid << rsLog;
            }
            catch(VoteException&) { 
                log() << "replSet voting no already voted for another" << rsLog;
            }
        }

        b.append("vote", vote);
        b.append("round", round);
    }

    void ReplSetImpl::getTargets(list<Target>& L) { 
        for( Member *m = head(); m; m=m->next() )
            if( m->hbinfo().up() )
                L.push_back( Target(m->fullName()) );
    }

    /* allUp only meaningful when true returned! */
    bool Consensus::weAreFreshest(bool& allUp) {
        BSONObj cmd = BSON(
               "replSetFresh" << 1 <<
               "set" << rs.name() << 
               "who" << rs._self->fullName() << 
               "cfgver" << rs._cfg->version );
        list<Target> L;
        rs.getTargets(L);
        multiCommand(cmd, L);
        int nok = 0;
        allUp = true;
        for( list<Target>::iterator i = L.begin(); i != L.end(); i++ ) {
            if( i->ok ) {
                nok++;
                if( i->result["fresher"].trueValue() )
                    return false;
            }
            else {
                log() << "replSet TEMP freshest returns " << i->result.toString() << rsLog;
                allUp = false;
            }
        }
        log() << "replSet TEMP we are freshest of up nodes, nok:" << nok << rsLog; 
        return true;
    }

    extern time_t started;

    void Consensus::_electSelf() {
        bool allUp;
        if( !weAreFreshest(allUp) ) { 
            log() << "replSet info not electing self, we are not freshest" << rsLog;
            return;
        }
        if( !allUp && time(0) - started < 60 * 5 ) { 
            log() << "replSet info not electing self, not all members up and we have been up less than 5 minutes" << rsLog;
        }

        time_t start = time(0);
        Member& me = *rs._self;        
        int tally = yea( me.id() );
        log() << "replSet info electSelf" << rsLog;

        BSONObj electCmd = BSON(
               "replSetElect" << 1 <<
               "set" << rs.name() << 
               "who" << me.fullName() << 
               "whoid" << me.hbinfo().id() << 
               "cfgver" << rs._cfg->version << 
               "round" << OID::gen() /* this is just for diagnostics */
            );

        list<Target> L;
        rs.getTargets(L);
        multiCommand(electCmd, L);

        for( list<Target>::iterator i = L.begin(); i != L.end(); i++ ) {
            log() << "replSet TEMP elect res: " << i->result.toString() << rsLog;
            Target& t = *i;
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
            log() << "replSet election succeeded assuming primary role" << rsLog;
            rs.assumePrimary();
            return;
        } 
        else { 
            log() << "replSet couldn't elect self, only received " << tally << " votes" << rsLog;
        }
    }

    void Consensus::electSelf() {
        try { 
            _electSelf(); 
        } 
        catch(VoteException& ) { 
            log() << "replSet not trying to elect self as responded yea to someone else recently" << rsLog;
        }
        catch(...) { }
    }

}
