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

            if( cmdObj["set"].String() != theReplSet->name() ) { 
                errmsg = "wrong repl set name";
                return false;
            }
            string who = cmdObj["who"].String();
            int cfgver = cmdObj["cfgver"].Int();
			unsigned long long ord = (unsigned long long) cmdObj["ord"].Long();

            bool weAreFresher = false;
            if( theReplSet->config().version > cfgver ) { 
                log() << "replSet member " << who << " is not yet aware its cfg version " << cfgver << " is stale" << rsLog;
				result.append("info", "config version stale");
                weAreFresher = true;
            }
			else if( ord > rsOpTime.ord )  { 
				weAreFresher = true;
			}
            result.append("ord", (long long) ord);
            result.append("fresher", weAreFresher);
            return true;
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

    void ReplSetImpl::_getTargets(list<Target>& L, int& configVersion) {
        configVersion = config().version;
        for( Member *m = head(); m; m=m->next() )
            if( m->hbinfo().up() )
                L.push_back( Target(m->fullName()) );
    }

    /* config version is returned as it is ok to use this unlocked.  BUT, if unlocked, you would need 
       to check later that the config didn't change. */
    void ReplSetImpl::getTargets(list<Target>& L, int& configVersion) {
        if( lockedByMe() ) { 
            _getTargets(L, configVersion);
            return;
        }
        lock lk(this);
        _getTargets(L, configVersion);
    }

    /* Do we have the newest data of them all?
       @param allUp - set to true if all members are up.  Only set if true returned.
       @return true if we are freshest.  Note we may tie.
    */
    bool Consensus::weAreFreshest(bool& allUp, int& nTies) {
		const unsigned long long ord = rsOpTime.ord;
        nTies = 0;
        cout << "TEMP COMMENTED OUT LINE IN consensus.cpp" << endl;
		//assert( ord > 0 );
        BSONObj cmd = BSON(
               "replSetFresh" << 1 <<
               "set" << rs.name() << 
			   "opTime" << (long long) rsOpTime.ord <<
               "who" << rs._self->fullName() << 
               "cfgver" << rs._cfg->version );
        list<Target> L;
        int ver;
        rs.getTargets(L, ver);
        multiCommand(cmd, L);
        int nok = 0;
        allUp = true;
        for( list<Target>::iterator i = L.begin(); i != L.end(); i++ ) {
            if( i->ok ) {
                nok++;
                if( i->result["fresher"].trueValue() )
                    return false;
                unsigned long long remoteOrd = (unsigned long long) i->result["ord"].Long();
                if( remoteOrd == ord )
                    nTies++;
                assert( remoteOrd <= ord );
            }
            else {
                DEV log() << "replSet freshest returns " << i->result.toString() << rsLog;
                allUp = false;
            }
        }
        DEV log() << "replSet dev we are freshest of up nodes, nok:" << nok << " nTies:" << nTies << rsLog; 
		assert( ord == rsOpTime.ord );
        return true;
    }

    extern time_t started;

    void Consensus::multiCommand(BSONObj cmd, list<Target>& L) { 
        assert( !rs.lockedByMe() );
        mongo::multiCommand(cmd, L);
    }

    void Consensus::_electSelf() {
        bool allUp;
        int nTies;
        if( !weAreFreshest(allUp, nTies) ) { 
            log() << "replSet info not electing self, we are not freshest" << rsLog;
            return;
        }
        if( !allUp && time(0) - started < 60 * 5 ) { 
            /* the idea here is that if a bunch of nodes bounce all at once, we don't want to drop data 
               if we don't have to -- we'd rather be offline and wait a little longer instead 
               todo: make this configurable.
               */
            log() << "replSet info not electing self, not all members up and we have been up less than 5 minutes" << rsLog;
            return;
        }

        Member& me = *rs._self;

        if( nTies ) {
            /* tie?  we then randomly sleep to try to not collide on our voting. */
            /* todo: smarter. */
            DEV log() << "replSet tie " << nTies << " sleeping a little" << rsLog;
            if( me.id() == 0 ) {
                // would be fine for one node not to sleep 
                // todo: biggest / highest priority nodes should be the ones that get to not sleep
            } else {
                assert( !rs.lockedByMe() ); // would be bad to go to sleep locked
                sleepmillis( ((unsigned) rand()) * 1000 + 50 );
                throw RetryAfterSleepException();
            }
        }

        time_t start = time(0);
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

        int configVersion;
        list<Target> L;
        rs.getTargets(L, configVersion);
        multiCommand(electCmd, L);

        {
            RSBase::lock lk(&rs);
            for( list<Target>::iterator i = L.begin(); i != L.end(); i++ ) {
                DEV log() << "replSet elect res: " << i->result.toString() << rsLog;
                if( i->ok ) {
                    int v = i->result["vote"].Int();
                    tally += v;
                }
            }
            if( tally*2 <= totalVotes() ) {
                log() << "replSet couldn't elect self, only received " << tally << " votes" << rsLog;
            }
            else if( time(0) - start > 30 ) {
                // defensive; should never happen as we have timeouts on connection and operation for our conn
                log() << "replSet too much time passed during our election, ignoring result" << rsLog;
            }
            else if( configVersion != rs.config().version ) { 
                log() << "replSet config version changed during our election, ignoring result" << rsLog;
            }
            else {
                /* succeeded. */
                log() << "replSet election succeeded, assuming primary role" << rsLog;
                rs.assumePrimary();
            } 
        }
    }

    void Consensus::electSelf() {
        assert( !rs.lockedByMe() );
        try { 
            _electSelf(); 
        } 
        catch(VoteException& ) { 
            log() << "replSet not trying to elect self as responded yea to someone else recently" << rsLog;
        }
        catch(...) { }
    }

}
