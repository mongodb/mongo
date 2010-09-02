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
#include "../../util/sock.h"
#include "../client.h"
#include "../../client/dbclient.h"
#include "../dbhelpers.h"
#include "rs.h"

namespace mongo { 

    using namespace bson;

    bool replSet = false;
    ReplSet *theReplSet = 0;
    extern string *discoveredSeed;

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
        assert( iAmPotentiallyHot() );
        writelock lk("admin."); // so we are synchronized with _logOp()
        box.setSelfPrimary(_self);
        //log() << "replSet PRIMARY" << rsLog; // self (" << _self->id() << ") is now primary" << rsLog;
    }

    void ReplSetImpl::changeState(MemberState s) { box.change(s, _self); }

    void ReplSetImpl::relinquish() { 
        if( box.getState().primary() ) {
            log() << "replSet relinquishing primary state" << rsLog;
            changeState(MemberState::RS_RECOVERING);
            
            /* close sockets that were talking to us */
            /*log() << "replSet closing sockets after reqlinquishing primary" << rsLog;
            MessagingPort::closeAllSockets(1);*/

            // todo: >
            //changeState(MemberState::RS_SECONDARY);
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

    bool ReplSetImpl::_stepDown() { 
        lock lk(this);
        if( box.getState().primary() ) { 
            changeState(MemberState::RS_RECOVERING);
            elect.steppedDown = time(0) + 60;
            log() << "replSet info stepped down as primary" << rsLog;
            return true;
        }
        return false;
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
        if( m->config().hidden )
            return;

        if( m->potentiallyHot() ) {
            hosts.push_back(m->h().toString());
        }
        else if( !m->config().arbiterOnly ) {
            if( m->config().slaveDelay ) {
                /* hmmm - we don't list these as they are stale. */   
            } else {
                passives.push_back(m->h().toString());
            }
        }
        else {
            arbiters.push_back(m->h().toString());
        }
    }

    void ReplSetImpl::_fillIsMaster(BSONObjBuilder& b) {
        const StateBox::SP sp = box.get();
        bool isp = sp.state.primary();
        b.append("setName", name());
        b.append("ismaster", isp);
        b.append("secondary", sp.state.secondary());
        {
            vector<string> hosts, passives, arbiters;
            _fillIsMasterHost(_self, hosts, passives, arbiters);

            for( Member *m = _members.head(); m; m = m->next() ) {
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
        if( myConfig().arbiterOnly )
            b.append("arbiterOnly", true);
        if( myConfig().slaveDelay )
            b.append("slaveDelay", myConfig().slaveDelay);
        if( myConfig().hidden )
            b.append("hidden", true);
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
                } else
                    seeds.push_back(m);
                if( *comma == 0 )
                    break;
                p = comma + 1;
            }
        }
    }

    ReplSetImpl::ReplSetImpl(ReplSetCmdline& replSetCmdline) : elect(this), 
        _self(0), 
        mgr( new Manager(this) )
    {
        _cfg = 0;
        memset(_hbmsg, 0, sizeof(_hbmsg));
        *_hbmsg = '.'; // temp...just to see
        lastH = 0;
        changeState(MemberState::RS_STARTUP);

        _seeds = &replSetCmdline.seeds;
        //for( vector<HostAndPort>::iterator i = seeds->begin(); i != seeds->end(); i++ )
        //    addMemberIfMissing(*i);

        log(1) << "replSet beginning startup..." << rsLog;

        loadConfig();

        unsigned sss = replSetCmdline.seedSet.size();
        for( Member *m = head(); m; m = m->next() ) {
            replSetCmdline.seedSet.erase(m->h());
        }
        for( set<HostAndPort>::iterator i = replSetCmdline.seedSet.begin(); i != replSetCmdline.seedSet.end(); i++ ) {
            if( i->isSelf() ) {
                if( sss == 1 ) 
                    log(1) << "replSet warning self is listed in the seed list and there are no other seeds listed did you intend that?" << rsLog;
            } else
                log() << "replSet warning command line seed " << i->toString() << " is not present in the current repl set config" << rsLog;
        }
    }

    void newReplUp();

    void ReplSetImpl::loadLastOpTimeWritten() { 
        //assert( lastOpTimeWritten.isNull() );
        readlock lk(rsoplog);
        BSONObj o;
        if( Helpers::getLast(rsoplog, o) ) { 
            lastH = o["h"].numberLong();
            lastOpTimeWritten = o["ts"]._opTime();
            uassert(13290, "bad replSet oplog entry?", !lastOpTimeWritten.isNull());
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
    string ReplSetImpl::startupStatusMsg;

    extern BSONObj *getLastErrorDefault;

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

        list<const ReplSetConfig::MemberCfg*> newOnes;
        bool additive = reconf;
        {
            unsigned nfound = 0;
            int me = 0;
            for( vector<ReplSetConfig::MemberCfg>::iterator i = c.members.begin(); i != c.members.end(); i++ ) { 
                const ReplSetConfig::MemberCfg& m = *i;
                if( m.h.isSelf() ) {
                    nfound++;
                    me++;
                    if( !reconf || (_self && _self->id() == (unsigned) m._id) )
                        ;
                    else { 
                        log() << "replSet " << _self->id() << ' ' << m._id << rsLog;
                        assert(false);
                    }
                }
                else if( reconf ) { 
                    const Member *old = findById(m._id);
                    if( old ) { 
                        nfound++;
                        assert( (int) old->id() == m._id );
                        if( old->config() == m ) { 
                            additive = false;
                        }
                    }
                    else {
                        newOnes.push_back(&m);
                    }
                }
            }
            if( me == 0 ) {
                // log() << "replSet config : " << _cfg->toString() << rsLog;
                log() << "replSet error can't find self in the repl set configuration:" << rsLog;
                log() << c.toString() << rsLog;
                assert(false);
            }
            uassert( 13302, "replSet error self appears twice in the repl set configuration", me<=1 );

            if( reconf && config().members.size() != nfound ) 
                additive = false;
        }

        _cfg = new ReplSetConfig(c);
        assert( _cfg->ok() );
        assert( _name.empty() || _name == _cfg->_id );
        _name = _cfg->_id;
        assert( !_name.empty() );

        if( additive ) { 
            log() << "replSet info : additive change to configuration" << rsLog;
            for( list<const ReplSetConfig::MemberCfg*>::const_iterator i = newOnes.begin(); i != newOnes.end(); i++ ) {
                const ReplSetConfig::MemberCfg* m = *i;
                Member *mi = new Member(m->h, m->_id, m, false);

                /** we will indicate that new members are up() initially so that we don't relinquish our 
                    primary state because we can't (transiently) see a majority.  they should be up as we 
                    check that new members are up before getting here on reconfig anyway.
                    */
                mi->get_hbinfo().health = 0.1;

                _members.push(mi);
                startHealthTaskFor(mi);
            }
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
        _self = 0;
        for( vector<ReplSetConfig::MemberCfg>::iterator i = _cfg->members.begin(); i != _cfg->members.end(); i++ ) { 
            const ReplSetConfig::MemberCfg& m = *i;
            Member *mi;
            if( m.h.isSelf() ) {
                assert( _self == 0 );
                mi = _self = new Member(m.h, m._id, &m, true);
                if( (int)mi->id() == oldPrimaryId )
                    box.setSelfPrimary(mi);
            } else {
                mi = new Member(m.h, m._id, &m, false);
                _members.push(mi);
                startHealthTaskFor(mi);
                if( (int)mi->id() == oldPrimaryId )
                    box.setOtherPrimary(mi);
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
            if( ++n == 1 ) myVersion = cfg.version;
            if( cfg.ok() && cfg.version > v ) { 
                highest = &cfg;
                v = cfg.version;
            }
        }
        assert( highest );

        if( !initFromConfig(*highest) ) 
            return false;

        if( highest->version > myVersion && highest->version >= 0 ) { 
            log() << "replSet got config version " << highest->version << " from a remote, saving locally" << rsLog;
            writelock lk("admin.");
            highest->saveConfigLocally(BSONObj());
        }
        return true;
    }

    void ReplSetImpl::loadConfig() {
        while( 1 ) {
            startupStatus = LOADINGCONFIG;
            startupStatusMsg = "loading " + rsConfigNs + " config (LOADINGCONFIG)";
            try {
                vector<ReplSetConfig> configs;
                try { 
                    configs.push_back( ReplSetConfig(HostAndPort::me()) );
                }
                catch(DBException& e) {
                    log() << "replSet exception loading our local replset configuration object : " << e.toString() << rsLog;
                    throw;
                }
                for( vector<HostAndPort>::const_iterator i = _seeds->begin(); i != _seeds->end(); i++ ) {
                    try { 
                        configs.push_back( ReplSetConfig(*i) );
                    }
                    catch( DBException& e ) { 
                        log() << "replSet exception trying to load config from " << *i << " : " << e.toString() << rsLog;
                    }
                }

                if( discoveredSeed ) { 
                    try {
                        configs.push_back( ReplSetConfig(HostAndPort(*discoveredSeed)) );
                    }
                    catch( DBException& ) { 
                        log(1) << "replSet exception trying to load config from discovered seed " << *discoveredSeed << rsLog;
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
                        startupStatusMsg = "can't get " + rsConfigNs + " config from self or any seed (EMPTYCONFIG)";
                        log() << "replSet can't get " << rsConfigNs << " config from self or any seed (EMPTYCONFIG)" << rsLog;
                        log(1) << "replSet have you run replSetInitiate yet?" << rsLog;
                        if( _seeds->size() == 0 )
                            log(1) << "replSet info no seed hosts were specified on the --replSet command line" << rsLog;
                    }
                    else {
                        startupStatus = EMPTYUNREACHABLE;
                        startupStatusMsg = "can't currently get " + rsConfigNs + " config from self or any seed (EMPTYUNREACHABLE)";
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
                startupStatusMsg = "replSet error loading set config (BADCONFIG)";
                log() << "replSet error loading configurations " << e.toString() << rsLog;
                log() << "replSet error replication will not start" << rsLog;
                sethbmsg("error loading set config");
                _fatal();
                throw;
            }
            break;
        }
        startupStatusMsg = "? started";
        startupStatus = STARTED;
    }

    void ReplSetImpl::_fatal() 
    { 
        //lock l(this);
        box.set(MemberState::RS_FATAL, 0);
        //sethbmsg("fatal error");
        log() << "replSet error fatal, stopping replication" << rsLog; 
    }

    void ReplSet::haveNewConfig(ReplSetConfig& newConfig, bool addComment) { 
        lock l(this); // convention is to lock replset before taking the db rwlock
        writelock lk("");
        bo comment;
        if( addComment )
            comment = BSON( "msg" << "Reconfig set" << "version" << newConfig.version );
        newConfig.saveConfigLocally(comment);
        try { 
            initFromConfig(newConfig, true);
            log() << "replSet replSetReconfig new config saved locally" << rsLog;
        }
        catch(DBException& e) { 
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
        Client::initThread("startReplSets");
        try { 
            assert( theReplSet == 0 );
            if( replSetCmdline == 0 ) {
                assert(!replSet);
                return;
            }
            (theReplSet = new ReplSet(*replSetCmdline))->go();
        }
        catch(std::exception& e) { 
            log() << "replSet caught exception in startReplSets thread: " << e.what() << rsLog;
            if( theReplSet ) 
                theReplSet->fatal();
        }
        cc().shutdown();
    }

}

namespace boost { 

    void assertion_failed(char const * expr, char const * function, char const * file, long line)
    {
        mongo::log() << "boost assertion failure " << expr << ' ' << function << ' ' << file << ' ' << line << endl;
    }

}
