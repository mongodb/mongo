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
#include "replset.h"

namespace mongo { 

    bool replSet = false;
    ReplSet *theReplSet = 0;

    void ReplSet::fillIsMaster(BSONObjBuilder& b) {
        log() << "hellO" << rsLog;
        b.append("ismaster", 0);
        b.append("ok", false);
        b.append("msg", "not yet implemented");
    }

    /** @param cfgString <setname>/<seedhost1>,<seedhost2> */
/*
    ReplSet::ReplSet(string cfgString) : fatal(false) {

    }
*/
    /** @param cfgString <setname>/<seedhost1>,<seedhost2> */
    ReplSet::ReplSet(string cfgString) : _self(0), elect(this), _mgr(this) {
        _myState = STARTUP;
        _currentPrimary = 0;

        const char *p = cfgString.c_str(); 
        const char *slash = strchr(p, '/');
        uassert(13093, "bad --replSet config string format is: <setname>/<seedhost1>,<seedhost2>[,...]", slash != 0 && p != slash);
        _name = string(p, slash-p);

        log() << "replSet startup " << cfgString << rsLog;

        set<HostAndPort> seedSet;
        vector<HostAndPort> *seeds = new vector<HostAndPort>;
        p = slash + 1;
        while( 1 ) {
            const char *comma = strchr(p, ',');
            if( comma == 0 ) comma = strchr(p,0);
            uassert(13094, "bad --replSet config string", p != comma);
            {
                HostAndPort m;
                try {
                    m = HostAndPort::fromString(string(p, comma-p));
                }
                catch(...) {
                    uassert(13114, "bad --replSet seed hostname", false);
                }
                uassert(13096, "bad --replSet config string - dups?", seedSet.count(m) == 0 );
                seedSet.insert(m);
                uassert(13101, "can't use localhost in replset host list", !m.isLocalHost());
                if( m.isSelf() )
                    log() << "replSet ignoring seed " << m.toString() << " (=self)" << rsLog;
                else
                    seeds->push_back(m);
                if( *comma == 0 )
                    break;
                p = comma + 1;
            }
        }

        _seeds = seeds;
        //for( vector<HostAndPort>::iterator i = seeds->begin(); i != seeds->end(); i++ )
        //    addMemberIfMissing(*i);

        log() << "replSet load config from various servers..." << rsLog;

        loadConfig();

        for( Member *m = head(); m; m = m->next() )
            seedSet.erase(m->_h);
        for( set<HostAndPort>::iterator i = seedSet.begin(); i != seedSet.end(); i++ ) {
            log() << "replSet warning: command line seed " << i->toString() << " is not present in the current repl set config" << rsLog;
        }
    
    }

    ReplSet::StartupStatus ReplSet::startupStatus = PRESTART;
    string ReplSet::startupStatusMsg;

    void ReplSet::setFrom(ReplSetConfig& c) { 
        _cfg.reset( new ReplSetConfig(c) );
        assert( _cfg->ok() );
        assert( _name.empty() || _name == _cfg->_id );
        _name = _cfg->_id;
        assert( !_name.empty() );

        assert( _members.head() == 0 );
        int me=0;
        for( vector<ReplSetConfig::MemberCfg>::iterator i = _cfg->members.begin(); i != _cfg->members.end(); i++ ) { 
            const ReplSetConfig::MemberCfg& m = *i;
            if( m.h.isSelf() ) {
                me++;
                assert( _self == 0 );
                _self = new Member(m.h, m._id, &m);
            } else {
                Member *mi = new Member(m.h, m._id, &m);
                _members.push(mi);
            }
        }
        assert( me == 1 );
    }

    void ReplSet::finishLoadingConfig(vector<ReplSetConfig>& cfgs) { 
        int v = -1;
        ReplSetConfig *highest = 0;
        for( vector<ReplSetConfig>::iterator i = cfgs.begin(); i != cfgs.end(); i++ ) { 
            ReplSetConfig& cfg = *i;
            if( cfg.ok() && cfg.version > v ) { 
                highest = &cfg;
                v = cfg.version;
            }
        }
        assert( highest );
        setFrom(*highest);
    }

    void ReplSet::loadConfig() {
        while( 1 ) {
            startupStatus = LOADINGCONFIG;
            startupStatusMsg = "loading " + rsConfigNs + " config (LOADINGCONFIG)";
            try {
                vector<ReplSetConfig> configs;
                configs.push_back( ReplSetConfig(HostAndPort::me()) );
                for( vector<HostAndPort>::const_iterator i = _seeds->begin(); i != _seeds->end(); i++ ) {
                    configs.push_back( ReplSetConfig(*i) );
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
                        startupStatusMsg = "can't get " + rsConfigNs + " config from self or any seed (uninitialized?)";
                        log() << "replSet can't get " << rsConfigNs << " config from self or any seed (EMPTYCONFIG)" << rsLog;
                        log() << "replSet have you ran replSetInitiate yet?" << rsLog;
                        log() << "replSet sleeping 1 minute and will try again." << rsLog;
                    }
                    else {
                        startupStatus = EMPTYUNREACHABLE;
                        startupStatusMsg = "can't currently get " + rsConfigNs + " config from self or any seed (EMPTYUNREACHABLE)";
                        log() << "replSet can't get " << rsConfigNs << " config from self or any seed." << rsLog;
                        log() << "replSet sleeping 1 minute and will try again." << rsLog;
                    }

                    sleepsecs(60);
                    continue;
                }
                finishLoadingConfig(configs);
            }
            catch(DBException& e) { 
                startupStatus = BADCONFIG;
                startupStatusMsg = "replSet error loading set config (BADCONFIG)";
                log() << "replSet error loading configurations " << e.toString() << rsLog;
                log() << "replSet replication will not start" << rsLog;
                fatal();
                throw;
            }
            break;
        }
        startupStatusMsg = "? started";
        startupStatus = STARTED;
    }

    /* called at startup */
    void startReplSets() {
        mongo::lastError.reset( new LastError() );
        try { 
            assert( theReplSet == 0 );
            if( cmdLine.replSet.empty() ) {
                assert(!replSet);
                return;
            }
            (theReplSet = new ReplSet(cmdLine.replSet))->go();
        }
        catch(std::exception& e) { 
            log() << "replSet Caught exception in management thread: " << e.what() << rsLog;
            if( theReplSet ) 
                theReplSet->fatal();
        }
    }

}
