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

#include "pch.h"
#include "../cmdline.h"
#include "../../util/net/sock.h"
#include "../client.h"
#include "../dbhelpers.h"
#include "../../s/d_logic.h"
#include "rs.h"
#include "connections.h"
#include "../repl.h"
#include "../instance.h"

using namespace std;

namespace mongo {
    
    using namespace bson;

    bool replSet = false;
    ReplSet *theReplSet = 0;

    bool isCurrentlyAReplSetPrimary() { 
        return theReplSet && theReplSet->isPrimary();
    }

    void replset::sethbmsg(const string& s, const int level) {
        if (theReplSet) {
            theReplSet->sethbmsg(s, logLevel);
        }
    }

    void ReplSetImpl::sethbmsg(string s, int logLevel) {
        static time_t lastLogged;
        _hbmsgTime = time(0);

        if( s == _hbmsg ) {
            // unchanged
            if( _hbmsgTime - lastLogged < 60 )
                return;
        }

        unsigned sz = s.size();
        if( sz >= 256 )
            memcpy(_hbmsg, s.c_str(), 255);
        else {
            _hbmsg[sz] = 0;
            memcpy(_hbmsg, s.c_str(), sz);
        }
        if( !s.empty() ) {
            lastLogged = _hbmsgTime;
            log(logLevel) << "replSet " << s << rsLog;
        }
    }

    void ReplSetImpl::assumePrimary() {
        LOG(2) << "replSet assuming primary" << endl;
        verify( iAmPotentiallyHot() );
        // so we are synchronized with _logOp().  perhaps locking local db only would suffice, but until proven 
        // will take this route, and this is very rare so it doesn't matter anyway
        Lock::GlobalWrite lk; 

        // Make sure that new OpTimes are higher than existing ones even with clock skew
        DBDirectClient c;
        BSONObj lastOp = c.findOne( "local.oplog.rs", Query().sort(reverseNaturalObj), NULL, QueryOption_SlaveOk );
        if ( !lastOp.isEmpty() ) {
            OpTime::setLast( lastOp[ "ts" ].date() );
        }

        changeState(MemberState::RS_PRIMARY);
    }

    void ReplSetImpl::changeState(MemberState s) { box.change(s, _self); }

    void ReplSetImpl::setMaintenanceMode(const bool inc) {
        lock lk(this);

        if (inc) {
            log() << "replSet going into maintenance mode (" << _maintenanceMode << " other tasks)" << rsLog;

            _maintenanceMode++;
            changeState(MemberState::RS_RECOVERING);
        }
        else {
            _maintenanceMode--;
            // no need to change state, syncTail will try to go live as a secondary soon

            log() << "leaving maintenance mode (" << _maintenanceMode << " other tasks)" << rsLog;
        }
    }

    Member* ReplSetImpl::getMostElectable() {
        lock lk(this);

        Member *max = 0;
        set<unsigned>::iterator it = _electableSet.begin();
        while ( it != _electableSet.end() ) {
            const Member *temp = findById(*it);
            if (!temp) {
                log() << "couldn't find member: " << *it << endl;
                set<unsigned>::iterator it_delete = it;
                it++;
                _electableSet.erase(it_delete);
                continue;
            }
            if (!max || max->config().priority < temp->config().priority) {
                max = (Member*)temp;
            }
            it++;
        }

        return max;
    }

    void ReplSetImpl::relinquish() {
        LOG(2) << "replSet attempting to relinquish" << endl;
        if( box.getState().primary() ) {
            {
                Lock::DBWrite lk("admin."); // so we are synchronized with _logOp()
            
                log() << "replSet relinquishing primary state" << rsLog;
                changeState(MemberState::RS_SECONDARY);

                /* close sockets that were talking to us so they don't blithly send many writes that will fail
                   with "not master" (of course client could check result code, but in case they are not)
                */
                log() << "replSet closing client sockets after relinquishing primary" << rsLog;
                MessagingPort::closeAllSockets(1);
            }

            // now that all connections were closed, strip this mongod from all sharding details
            // if and when it gets promoted to a primary again, only then it should reload the sharding state
            // the rationale here is that this mongod won't bring stale state when it regains primaryhood
            shardingState.resetShardingState();

        }
        else if( box.getState().startup2() ) {
            // ? add comment
            changeState(MemberState::RS_RECOVERING);
        }
    }

    /* look freshly for who is primary - includes relinquishing ourself. */
    void ReplSetImpl::forgetPrimary() {
        if( box.getState().primary() )
            relinquish();
        else {
            box.setOtherPrimary(0);
        }
    }

    // for the replSetStepDown command
    bool ReplSetImpl::_stepDown(int secs) {
        lock lk(this);
        if( box.getState().primary() ) {
            elect.steppedDown = time(0) + secs;
            log() << "replSet info stepping down as primary secs=" << secs << rsLog;
            relinquish();
            return true;
        }
        return false;
    }

    bool ReplSetImpl::_freeze(int secs) {
        lock lk(this);
        /* note if we are primary we remain primary but won't try to elect ourself again until
           this time period expires.
           */
        if( secs == 0 ) {
            elect.steppedDown = 0;
            log() << "replSet info 'unfreezing'" << rsLog;
        }
        else {
            if( !box.getState().primary() ) {
                elect.steppedDown = time(0) + secs;
                log() << "replSet info 'freezing' for " << secs << " seconds" << rsLog;
            }
            else {
                log() << "replSet info received freeze command but we are primary" << rsLog;
            }
        }
        return true;
    }

    void ReplSetImpl::msgUpdateHBInfo(HeartbeatInfo h) {
        for( Member *m = _members.head(); m; m=m->next() ) {
            if( m->id() == h.id() ) {
                m->_hbinfo = h;
                return;
            }
        }
    }

    list<HostAndPort> ReplSetImpl::memberHostnames() const {
        list<HostAndPort> L;
        L.push_back(_self->h());
        for( Member *m = _members.head(); m; m = m->next() )
            L.push_back(m->h());
        return L;
    }

    void ReplSetImpl::_fillIsMasterHost(const Member *m, vector<string>& hosts, vector<string>& passives, vector<string>& arbiters) {
        verify( m );
        if( m->config().hidden )
            return;

        if( m->potentiallyHot() ) {
            hosts.push_back(m->h().toString());
        }
        else if( !m->config().arbiterOnly ) {
            if( m->config().slaveDelay ) {
                /* hmmm - we don't list these as they are stale. */
            }
            else {
                passives.push_back(m->h().toString());
            }
        }
        else {
            arbiters.push_back(m->h().toString());
        }
    }

    void ReplSetImpl::_fillIsMaster(BSONObjBuilder& b) {
        lock lk(this);
        
        const StateBox::SP sp = box.get();
        bool isp = sp.state.primary();
        b.append("setName", name());
        b.append("ismaster", isp);
        b.append("secondary", sp.state.secondary());
        {
            vector<string> hosts, passives, arbiters;
            _fillIsMasterHost(_self, hosts, passives, arbiters);

            for( Member *m = _members.head(); m; m = m->next() ) {
                verify( m );
                _fillIsMasterHost(m, hosts, passives, arbiters);
            }

            if( hosts.size() > 0 ) {
                b.append("hosts", hosts);
            }
            if( passives.size() > 0 ) {
                b.append("passives", passives);
            }
            if( arbiters.size() > 0 ) {
                b.append("arbiters", arbiters);
            }
        }

        if( !isp ) {
            const Member *m = sp.primary;
            if( m )
                b.append("primary", m->h().toString());
        }
        else {
            b.append("primary", _self->fullName());
        }

        if( myConfig().arbiterOnly )
            b.append("arbiterOnly", true);
        if( myConfig().priority == 0 && !myConfig().arbiterOnly)
            b.append("passive", true);
        if( myConfig().slaveDelay )
            b.append("slaveDelay", myConfig().slaveDelay);
        if( myConfig().hidden )
            b.append("hidden", true);
        if( !myConfig().buildIndexes )
            b.append("buildIndexes", false);
        if( !myConfig().tags.empty() ) {
            BSONObjBuilder a;
            for( map<string,string>::const_iterator i = myConfig().tags.begin(); i != myConfig().tags.end(); i++ )
                a.append((*i).first, (*i).second);
            b.append("tags", a.done());
        }
        b.append("me", myConfig().h.toString());
    }

    /** @param cfgString <setname>/<seedhost1>,<seedhost2> */

    void parseReplsetCmdLine(string cfgString, string& setname, vector<HostAndPort>& seeds, set<HostAndPort>& seedSet ) {
        const char *p = cfgString.c_str();
        const char *slash = strchr(p, '/');
        if( slash )
            setname = string(p, slash-p);
        else
            setname = p;
        uassert(13093, "bad --replSet config string format is: <setname>[/<seedhost1>,<seedhost2>,...]", !setname.empty());

        if( slash == 0 )
            return;

        p = slash + 1;
        while( 1 ) {
            const char *comma = strchr(p, ',');
            if( comma == 0 ) comma = strchr(p,0);
            if( p == comma )
                break;
            {
                HostAndPort m;
                try {
                    m = HostAndPort( string(p, comma-p) );
                }
                catch(...) {
                    uassert(13114, "bad --replSet seed hostname", false);
                }
                uassert(13096, "bad --replSet command line config string - dups?", seedSet.count(m) == 0 );
                seedSet.insert(m);
                //uassert(13101, "can't use localhost in replset host list", !m.isLocalHost());
                if( m.isSelf() ) {
                    log(1) << "replSet ignoring seed " << m.toString() << " (=self)" << rsLog;
                }
                else
                    seeds.push_back(m);
                if( *comma == 0 )
                    break;
                p = comma + 1;
            }
        }
    }

    ReplSetImpl::ReplSetImpl(ReplSetCmdline& replSetCmdline) : elect(this),
        _currentSyncTarget(0),
        _forceSyncTarget(0),
        _blockSync(false),
        _hbmsgTime(0),
        _self(0),
        _maintenanceMode(0),
        mgr( new Manager(this) ),
        ghost( new GhostSync(this) ) {

        _cfg = 0;
        memset(_hbmsg, 0, sizeof(_hbmsg));
        strcpy( _hbmsg , "initial startup" );
        lastH = 0;
        changeState(MemberState::RS_STARTUP);

        _seeds = &replSetCmdline.seeds;

        LOG(1) << "replSet beginning startup..." << rsLog;

        loadConfig();

        unsigned sss = replSetCmdline.seedSet.size();
        for( Member *m = head(); m; m = m->next() ) {
            replSetCmdline.seedSet.erase(m->h());
        }
        for( set<HostAndPort>::iterator i = replSetCmdline.seedSet.begin(); i != replSetCmdline.seedSet.end(); i++ ) {
            if( i->isSelf() ) {
                if( sss == 1 ) {
                    LOG(1) << "replSet warning self is listed in the seed list and there are no other seeds listed did you intend that?" << rsLog;
                }
            }
            else {
                log() << "replSet warning command line seed " << i->toString() << " is not present in the current repl set config" << rsLog;
            }
        }
    }

    void newReplUp();

    void ReplSetImpl::loadLastOpTimeWritten(bool quiet) {
        Lock::DBRead lk(rsoplog);
        BSONObj o;
        if( Helpers::getLast(rsoplog, o) ) {
            lastH = o["h"].numberLong();
            lastOpTimeWritten = o["ts"]._opTime();
            uassert(13290, "bad replSet oplog entry?", quiet || !lastOpTimeWritten.isNull());
        }
    }

    /* call after constructing to start - returns fairly quickly after launching its threads */
    void ReplSetImpl::_go() {
        try {
            loadLastOpTimeWritten();
        }
        catch(std::exception& e) {
            log() << "replSet error fatal couldn't query the local " << rsoplog << " collection.  Terminating mongod after 30 seconds." << rsLog;
            log() << e.what() << rsLog;
            sleepsecs(30);
            dbexit( EXIT_REPLICATION_ERROR );
            return;
        }

        changeState(MemberState::RS_STARTUP2);
        startThreads();
        newReplUp(); // oplog.cpp
    }

    ReplSetImpl::StartupStatus ReplSetImpl::startupStatus = PRESTART;
    DiagStr ReplSetImpl::startupStatusMsg;

    extern BSONObj *getLastErrorDefault;

    void ReplSetImpl::setSelfTo(Member *m) {
        // already locked in initFromConfig
        _self = m;
        _id = m->id();
        _config = m->config();
        if( m ) _buildIndexes = m->config().buildIndexes;
        else _buildIndexes = true;
    }

    /** @param reconf true if this is a reconfiguration and not an initial load of the configuration.
        @return true if ok; throws if config really bad; false if config doesn't include self
    */
    bool ReplSetImpl::initFromConfig(ReplSetConfig& c, bool reconf) {
        /* NOTE: haveNewConfig() writes the new config to disk before we get here.  So
                 we cannot error out at this point, except fatally.  Check errors earlier.
                 */
        lock lk(this);

        if( getLastErrorDefault || !c.getLastErrorDefaults.isEmpty() ) {
            // see comment in dbcommands.cpp for getlasterrordefault
            getLastErrorDefault = new BSONObj( c.getLastErrorDefaults );
        }

        list<ReplSetConfig::MemberCfg*> newOnes;
        // additive short-cuts the new config setup. If we are just adding a
        // node/nodes and nothing else is changing, this is additive. If it's
        // not a reconfig, we're not adding anything
        bool additive = reconf;
        {
            unsigned nfound = 0;
            int me = 0;
            for( vector<ReplSetConfig::MemberCfg>::iterator i = c.members.begin(); i != c.members.end(); i++ ) {
                
                ReplSetConfig::MemberCfg& m = *i;
                if( m.h.isSelf() ) {
                    me++;
                }
                
                if( reconf ) {
                    const Member *old = findById(m._id);
                    if( old ) {
                        nfound++;
                        verify( (int) old->id() == m._id );
                        if( old->config() != m ) {
                            additive = false;
                        }
                    }
                    else {
                        newOnes.push_back(&m);
                    }
                }
            }
            if( me == 0 ) { // we're not in the config -- we must have been removed
                if (state().shunned()) {
                    // already took note of our ejection from the set
                    // so just sit tight and poll again
                    return false;
                }

                _members.orphanAll();

                // kill off rsHealthPoll threads (because they Know Too Much about our past)
                endOldHealthTasks();

                // close sockets to force clients to re-evaluate this member
                MessagingPort::closeAllSockets(0);

                // take note of our ejection
                changeState(MemberState::RS_SHUNNED);

                // go into holding pattern
                log() << "replSet info self not present in the repl set configuration:" << rsLog;
                log() << c.toString() << rsLog;

                loadConfig();  // redo config from scratch
                return false; 
            }
            uassert( 13302, "replSet error self appears twice in the repl set configuration", me<=1 );

            // if we found different members that the original config, reload everything
            if( reconf && config().members.size() != nfound )
                additive = false;
        }

        _cfg = new ReplSetConfig(c);
        dassert( &config() == _cfg ); // config() is same thing but const, so we use that when we can for clarity below
        verify( config().ok() );
        verify( _name.empty() || _name == config()._id );
        _name = config()._id;
        verify( !_name.empty() );

        // this is a shortcut for simple changes
        if( additive ) {
            log() << "replSet info : additive change to configuration" << rsLog;
            for( list<ReplSetConfig::MemberCfg*>::const_iterator i = newOnes.begin(); i != newOnes.end(); i++ ) {
                ReplSetConfig::MemberCfg *m = *i;
                Member *mi = new Member(m->h, m->_id, m, false);

                /** we will indicate that new members are up() initially so that we don't relinquish our
                    primary state because we can't (transiently) see a majority.  they should be up as we
                    check that new members are up before getting here on reconfig anyway.
                    */
                mi->get_hbinfo().health = 0.1;

                _members.push(mi);
                startHealthTaskFor(mi);
            }

            // if we aren't creating new members, we may have to update the
            // groups for the current ones
            _cfg->updateMembers(_members);

            return true;
        }

        // start with no members.  if this is a reconfig, drop the old ones.
        _members.orphanAll();

        endOldHealthTasks();

        int oldPrimaryId = -1;
        {
            const Member *p = box.getPrimary();
            if( p )
                oldPrimaryId = p->id();
        }
        forgetPrimary();

        // not setting _self to 0 as other threads use _self w/o locking
        int me = 0;

        // For logging
        string members = "";

        for( vector<ReplSetConfig::MemberCfg>::const_iterator i = config().members.begin(); i != config().members.end(); i++ ) {
            const ReplSetConfig::MemberCfg& m = *i;
            Member *mi;
            members += ( members == "" ? "" : ", " ) + m.h.toString();
            if( m.h.isSelf() ) {
                verify( me++ == 0 );
                mi = new Member(m.h, m._id, &m, true);
                if (!reconf) {
                    log() << "replSet I am " << m.h.toString() << rsLog;
                }
                setSelfTo(mi);

                if( (int)mi->id() == oldPrimaryId )
                    box.setSelfPrimary(mi);
            }
            else {
                mi = new Member(m.h, m._id, &m, false);
                _members.push(mi);
                if( (int)mi->id() == oldPrimaryId )
                    box.setOtherPrimary(mi);
            }
        }

        if( me == 0 ){
            log() << "replSet warning did not detect own host in full reconfig, members " << members << " config: " << c << rsLog;
        }
        else {
            // Do this after we've found ourselves, since _self needs
            // to be set before we can start the heartbeat tasks
            for( Member *mb = _members.head(); mb; mb=mb->next() ) {
                startHealthTaskFor( mb );
            }
        }
        return true;
    }

    // Our own config must be the first one.
    bool ReplSetImpl::_loadConfigFinish(vector<ReplSetConfig>& cfgs) {
        int v = -1;
        ReplSetConfig *highest = 0;
        int myVersion = -2000;
        int n = 0;
        for( vector<ReplSetConfig>::iterator i = cfgs.begin(); i != cfgs.end(); i++ ) {
            ReplSetConfig& cfg = *i;
            DEV log(1) << n+1 << " config shows version " << cfg.version << rsLog; 
            if( ++n == 1 ) myVersion = cfg.version;
            if( cfg.ok() && cfg.version > v ) {
                highest = &cfg;
                v = cfg.version;
            }
        }
        verify( highest );

        if( !initFromConfig(*highest) )
            return false;

        if( highest->version > myVersion && highest->version >= 0 ) {
            log() << "replSet got config version " << highest->version << " from a remote, saving locally" << rsLog;
            highest->saveConfigLocally(BSONObj());
        }
        return true;
    }

    void ReplSetImpl::loadConfig() {
        startupStatus = LOADINGCONFIG;
        startupStatusMsg.set("loading " + rsConfigNs + " config (LOADINGCONFIG)");
        LOG(1) << "loadConfig() " << rsConfigNs << endl;

        while( 1 ) {
            try {
                vector<ReplSetConfig> configs;
                try {
                    configs.push_back( ReplSetConfig(HostAndPort::me()) );
                }
                catch(DBException& e) {
                    log() << "replSet exception loading our local replset configuration object : " << e.toString() << rsLog;
                }
                for( vector<HostAndPort>::const_iterator i = _seeds->begin(); i != _seeds->end(); i++ ) {
                    try {
                        configs.push_back( ReplSetConfig(*i) );
                    }
                    catch( DBException& e ) {
                        log() << "replSet exception trying to load config from " << *i << " : " << e.toString() << rsLog;
                    }
                }
                {
                    scoped_lock lck( replSettings.discoveredSeeds_mx );
                    if( replSettings.discoveredSeeds.size() > 0 ) {
                        for (set<string>::iterator i = replSettings.discoveredSeeds.begin(); 
                             i != replSettings.discoveredSeeds.end(); 
                             i++) {
                            try {
                                configs.push_back( ReplSetConfig(HostAndPort(*i)) );
                            }
                            catch( DBException& ) {
                                log(1) << "replSet exception trying to load config from discovered seed " << *i << rsLog;
                                replSettings.discoveredSeeds.erase(*i);
                            }
                        }
                    }
                }

                if (!replSettings.reconfig.isEmpty()) {
                    try {
                        configs.push_back(ReplSetConfig(replSettings.reconfig, true));
                    }
                    catch( DBException& re) {
                        log() << "replSet couldn't load reconfig: " << re.what() << rsLog;
                        replSettings.reconfig = BSONObj();
                    }
                }

                int nok = 0;
                int nempty = 0;
                for( vector<ReplSetConfig>::iterator i = configs.begin(); i != configs.end(); i++ ) {
                    if( i->ok() )
                        nok++;
                    if( i->empty() )
                        nempty++;
                }
                if( nok == 0 ) {

                    if( nempty == (int) configs.size() ) {
                        startupStatus = EMPTYCONFIG;
                        startupStatusMsg.set("can't get " + rsConfigNs + " config from self or any seed (EMPTYCONFIG)");
                        log() << "replSet can't get " << rsConfigNs << " config from self or any seed (EMPTYCONFIG)" << rsLog;
                        static unsigned once;
                        if( ++once == 1 ) {
                            log() << "replSet info you may need to run replSetInitiate -- rs.initiate() in the shell -- if that is not already done" << rsLog;
                        }
                        if( _seeds->size() == 0 ) {
                            LOG(1) << "replSet info no seed hosts were specified on the --replSet command line" << rsLog;
                        }
                    }
                    else {
                        startupStatus = EMPTYUNREACHABLE;
                        startupStatusMsg.set("can't currently get " + rsConfigNs + " config from self or any seed (EMPTYUNREACHABLE)");
                        log() << "replSet can't get " << rsConfigNs << " config from self or any seed (yet)" << rsLog;
                    }

                    sleepsecs(10);
                    continue;
                }

                if( !_loadConfigFinish(configs) ) {
                    log() << "replSet info Couldn't load config yet. Sleeping 20sec and will try again." << rsLog;
                    sleepsecs(20);
                    continue;
                }
            }
            catch(DBException& e) {
                startupStatus = BADCONFIG;
                startupStatusMsg.set("replSet error loading set config (BADCONFIG)");
                log() << "replSet error loading configurations " << e.toString() << rsLog;
                log() << "replSet error replication will not start" << rsLog;
                sethbmsg("error loading set config");
                _fatal();
                throw;
            }
            break;
        }
        startupStatusMsg.set("? started");
        startupStatus = STARTED;
    }

    void ReplSetImpl::_fatal() {
        box.set(MemberState::RS_FATAL, 0);
        log() << "replSet error fatal, stopping replication" << rsLog;
    }

    void ReplSet::haveNewConfig(ReplSetConfig& newConfig, bool addComment) {
        bo comment;
        if( addComment )
            comment = BSON( "msg" << "Reconfig set" << "version" << newConfig.version );

        newConfig.saveConfigLocally(comment);

        try {
            if (initFromConfig(newConfig, true)) {
                log() << "replSet replSetReconfig new config saved locally" << rsLog;
            }
        }
        catch(DBException& e) {
            if( e.getCode() == 13497 /* removed from set */ ) {
                cc().shutdown();
                dbexit( EXIT_CLEAN , "removed from replica set" ); // never returns
                verify(0);
            }
            log() << "replSet error unexpected exception in haveNewConfig() : " << e.toString() << rsLog;
            _fatal();
        }
        catch(...) {
            log() << "replSet error unexpected exception in haveNewConfig()" << rsLog;
            _fatal();
        }
    }

    void Manager::msgReceivedNewConfig(BSONObj o) {
        log() << "replset msgReceivedNewConfig version: " << o["version"].toString() << rsLog;
        ReplSetConfig c(o);
        if( c.version > rs->config().version )
            theReplSet->haveNewConfig(c, false);
        else {
            log() << "replSet info msgReceivedNewConfig but version isn't higher " <<
                  c.version << ' ' << rs->config().version << rsLog;
        }
    }

    /* forked as a thread during startup
       it can run quite a while looking for config.  but once found,
       a separate thread takes over as ReplSetImpl::Manager, and this thread
       terminates.
    */
    void startReplSets(ReplSetCmdline *replSetCmdline) {
        Client::initThread("rsStart");
        try {
            verify( theReplSet == 0 );
            if( replSetCmdline == 0 ) {
                verify(!replSet);
                return;
            }
            replLocalAuth();
            (theReplSet = new ReplSet(*replSetCmdline))->go();
        }
        catch(std::exception& e) {
            log() << "replSet caught exception in startReplSets thread: " << e.what() << rsLog;
            if( theReplSet )
                theReplSet->fatal();
        }
        cc().shutdown();
    }

    void replLocalAuth() {
        if ( noauth )
            return;
        cc().getAuthenticationInfo()->authorize("local","_repl");
    }
    

}

