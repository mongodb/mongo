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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/db/repl/consensus.h"

#include "mongo/db/global_optime.h"
#include "mongo/db/repl/multicmd.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/replset_commands.h"

namespace mongo {
namespace repl {

    /** the first cmd called by a node seeking election and it's a basic sanity 
        test: do any of the nodes it can reach know that it can't be the primary?
        */
    class CmdReplSetFresh : public ReplSetCommand {
    public:
        void help(stringstream& h) const { h << "internal"; }
        CmdReplSetFresh() : ReplSetCommand("replSetFresh") { }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::internal);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }

        virtual bool run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            unsigned id = cmdObj["id"].Int();
            std::string setName = cmdObj["set"].String();
            std::string who = cmdObj["who"].String();
            int cfgver = cmdObj["cfgver"].Int();
            OpTime opTime(cmdObj["opTime"].Date());

            Status status = getGlobalReplicationCoordinator()->processReplSetFresh(setName,
                                                                                   who,
                                                                                   id,
                                                                                   cfgver,
                                                                                   opTime,
                                                                                   &result);
            return appendCommandStatus(result, status);
        }
    } cmdReplSetFresh;

    class CmdReplSetElect : public ReplSetCommand {
    public:
        void help(stringstream& h) const { h << "internal"; }
        CmdReplSetElect() : ReplSetCommand("replSetElect") { }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::internal);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }
    private:
        virtual bool run(OperationContext* txn, const string& , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if( !check(errmsg, result) )
                return false;
            theReplSet->elect.electCmdReceived(cmdObj, &result);
            return true;
        }
    } cmdReplSetElect;

    void Consensus::electCmdReceived(BSONObj cmd, BSONObjBuilder* _b) {
        BSONObjBuilder& b = *_b;
        DEV log() << "replSet received elect msg " << cmd.toString() << rsLog;
        else LOG(2) << "replSet received elect msg " << cmd.toString() << rsLog;
        string set = cmd["set"].String();
        unsigned whoid = cmd["whoid"].Int();
        int cfgver = cmd["cfgver"].Int();
        OID round = cmd["round"].OID();
        int myver = rs.config().version;

        const Member* primary = rs.box.getPrimary();
        const Member* hopeful = rs.findById(whoid);
        const Member* highestPriority = rs.getMostElectable();

        int vote = 0;
        if( set != rs.name() ) {
            log() << "replSet error received an elect request for '" << set << "' but our set name is '" << rs.name() << "'" << rsLog;
        }
        else if( myver < cfgver ) {
            // we are stale.  don't vote
        }
        else if( myver > cfgver ) {
            // they are stale!
            log() << "replSet electCmdReceived info got stale version # during election" << rsLog;
            vote = -10000;
        }
        else if( !hopeful ) {
            log() << "replSet electCmdReceived couldn't find member with id " << whoid << rsLog;
            vote = -10000;
        }
        else if( primary && primary == rs._self) {
            log() << "I am already primary, " << hopeful->fullName()
                  << " can try again once I've stepped down" << rsLog;
            vote = -10000;
        }
        else if (primary) {
            log() << hopeful->fullName() << " is trying to elect itself but " <<
                  primary->fullName() << " is already primary" << rsLog;
            vote = -10000;
        }
        else if( highestPriority && highestPriority->config().priority > hopeful->config().priority) {
            log() << hopeful->fullName() << " has lower priority than " << highestPriority->fullName();
            vote = -10000;
        }
        else {
            try {
                vote = _yea(whoid);
                dassert( hopeful->id() == whoid );
                log() << "replSet info voting yea for " <<  hopeful->fullName() << " (" << whoid << ')' << rsLog;
            }
            catch(VoteException&) {
                log() << "replSet voting no for " << hopeful->fullName() << " already voted for another" << rsLog;
            }
        }

        b.append("vote", vote);
        b.append("round", round);
    }

    int Consensus::_totalVotes() const {
        static int complain = 0;
        int vTot = rs._self->config().votes;
        for( Member *m = rs.head(); m; m=m->next() )
            vTot += m->config().votes;
        if( vTot % 2 == 0 && vTot && complain++ == 0 )
            log() << "replSet warning: even number of voting members in replica set config - "
                     "add an arbiter or set votes to 0 on one of the existing members" << rsLog;
        return vTot;
    }

    bool Consensus::aMajoritySeemsToBeUp() const {
        int vUp = rs._self->config().votes;
        for( Member *m = rs.head(); m; m=m->next() )
            vUp += m->hbinfo().up() ? m->config().votes : 0;
        return vUp * 2 > _totalVotes();
    }

    bool Consensus::shouldRelinquish() const {
        int vUp = rs._self->config().votes;
        for( Member *m = rs.head(); m; m=m->next() ) {
            if (m->hbinfo().up()) {
                vUp += m->config().votes;
            }
        }

        // the manager will handle calling stepdown if another node should be
        // primary due to priority

        return !( vUp * 2 > _totalVotes() );
    }

    const time_t LeaseTime = 3;

    SimpleMutex Consensus::lyMutex("ly");

    unsigned Consensus::_yea(unsigned memberId) { /* throws VoteException */
        SimpleMutex::scoped_lock lk(lyMutex);
        LastYea &L = _ly;
        time_t now = time(0);
        if( L.when + LeaseTime >= now && L.who != memberId ) {
            LOG(1) << "replSet not voting yea for " << memberId <<
                   " voted for " << L.who << ' ' << now-L.when << " secs ago" << rsLog;
            throw VoteException();
        }
        L.when = now;
        L.who = memberId;
        return rs._self->config().votes;
    }

    /* we vote for ourself at start of election.  once it fails, we can cancel the lease we had in
       place instead of leaving it for a long time.
       */
    void Consensus::_electionFailed(unsigned meid) {
        SimpleMutex::scoped_lock lk(lyMutex);
        LastYea &L = _ly;
        DEV verify( L.who == meid ); // this may not always always hold, so be aware, but adding for now as a quick sanity test
        if( L.who == meid )
            L.when = 0;
    }

    void ReplSetImpl::_getTargets(list<Target>& L, int& configVersion) {
        configVersion = config().version;
        for( Member *m = head(); m; m=m->next() )
            if( m->hbinfo().maybeUp() )
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
    bool Consensus::_weAreFreshest(bool& allUp, int& nTies) {
        const OpTime ord = theReplSet->lastOpTimeWritten;
        nTies = 0;
        verify( !ord.isNull() );
        BSONObj cmd = BSON(
                          "replSetFresh" << 1 <<
                          "set" << rs.name() <<
                          "opTime" << Date_t(ord.asDate()) <<
                          "who" << rs._self->fullName() <<
                          "cfgver" << rs._cfg->version <<
                          "id" << rs._self->id());
        list<Target> L;
        int ver;
        /* the following queries arbiters, even though they are never fresh.  wonder if that makes sense.
           it doesn't, but it could, if they "know" what freshness it one day.  so consider removing
           arbiters from getTargets() here.  although getTargets is used elsewhere for elections; there
           arbiters are certainly targets - so a "includeArbs" bool would be necessary if we want to make
           not fetching them herein happen.
           */
        rs.getTargets(L, ver);
        _multiCommand(cmd, L);
        int nok = 0;
        allUp = true;
        for( list<Target>::iterator i = L.begin(); i != L.end(); i++ ) {
            if( i->ok ) {
                nok++;
                if( i->result["fresher"].trueValue() ) {
                    log() << "not electing self, we are not freshest" << rsLog;
                    return false;
                }
                OpTime remoteOrd( i->result["opTime"].Date() );
                if( remoteOrd == ord )
                    nTies++;
                verify( remoteOrd <= ord );

                if( i->result["veto"].trueValue() ) {
                    BSONElement msg = i->result["errmsg"];
                    if (!msg.eoo()) {
                        log() << "not electing self, " << i->toHost << " would veto with '" <<
                            msg.String() << "'" << rsLog;
                    }
                    else {
                        log() << "not electing self, " << i->toHost << " would veto" << rsLog;
                    }
                    return false;
                }
            }
            else {
                DEV log() << "replSet freshest returns " << i->result.toString() << rsLog;
                allUp = false;
            }
        }
        LOG(1) << "replSet dev we are freshest of up nodes, nok:" << nok << " nTies:" << nTies << rsLog;
        verify( ord <= theReplSet->lastOpTimeWritten ); // <= as this may change while we are working...
        return true;
    }

    void Consensus::_multiCommand(BSONObj cmd, list<Target>& L) {
        verify( !rs.lockedByMe() );
        multiCommand(cmd, L);
    }

    void Consensus::_electSelf() {
        if( time(0) < steppedDown )
            return;

        {
            const OpTime ord = theReplSet->lastOpTimeWritten;
            if( ord == 0 ) {
                log() << "replSet info not trying to elect self, do not yet have a complete set of data from any point in time" << rsLog;
                return;
            }
        }

        bool allUp;
        int nTies;
        if( !_weAreFreshest(allUp, nTies) ) {
            return;
        }

        rs.sethbmsg("",9);

        if (!allUp && time(0) - serverGlobalParams.started < 60 * 5) {
            /* the idea here is that if a bunch of nodes bounce all at once, we don't want to drop data
               if we don't have to -- we'd rather be offline and wait a little longer instead
               todo: make this configurable.
               */
            rs.sethbmsg("not electing self, not all members up and we have been up less than 5 minutes");
            return;
        }

        Member& me = *rs._self;

        if( nTies ) {
            /* tie?  we then randomly sleep to try to not collide on our voting. */
            /* todo: smarter. */
            if( me.id() == 0 || _sleptLast ) {
                // would be fine for one node not to sleep
                // todo: biggest / highest priority nodes should be the ones that get to not sleep
            }
            else {
                verify( !rs.lockedByMe() ); // bad to go to sleep locked
                unsigned ms = ((unsigned) rand()) % 1000 + 50;
                DEV log() << "replSet tie " << nTies << " sleeping a little " << ms << "ms" << rsLog;
                _sleptLast = true;
                sleepmillis(ms);
                throw RetryAfterSleepException();
            }
        }
        _sleptLast = false;

        time_t start = time(0);
        unsigned meid = me.id();
        int tally = _yea( meid );
        bool success = false;
        try {
            log() << "replSet info electSelf " << meid << rsLog;

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
            _multiCommand(electCmd, L);

            {
                for( list<Target>::iterator i = L.begin(); i != L.end(); i++ ) {
                    LOG(1) << "replSet elect res: " << i->result.toString() << rsLog;
                    if( i->ok ) {
                        int v = i->result["vote"].Int();
                        tally += v;
                    }
                }
                if( tally*2 <= _totalVotes() ) {
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
                    LOG(1) << "replSet election succeeded, assuming primary role" << rsLog;
                    success = true;

                    setElectionTime(getNextGlobalOptime());

                    rs._assumePrimary();
                }
            }
        }
        catch( std::exception& ) {
            if( !success ) _electionFailed(meid);
            throw;
        }
        if( !success ) _electionFailed(meid);
    }

    void Consensus::electSelf() {
        verify( !rs.lockedByMe() );
        verify( !rs.myConfig().arbiterOnly );
        verify( rs.myConfig().slaveDelay == 0 );
        try {
            _electSelf();
        }
        catch(RetryAfterSleepException&) {
            throw;
        }
        catch(VoteException& ) {
            log() << "replSet not trying to elect self as responded yea to someone else recently" << rsLog;
        }
        catch(DBException& e) {
            log() << "replSet warning caught unexpected exception in electSelf() " << e.toString() << rsLog;
        }
        catch(...) {
            log() << "replSet warning caught unexpected exception in electSelf()" << rsLog;
        }
    }

} // namespace repl
} // namespace mongo
