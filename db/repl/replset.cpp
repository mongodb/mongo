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
    ReplSet::ReplSet(string cfgString) : _self(0), elect(this) {
        _myState = STARTUP;

        const char *p = cfgString.c_str(); 
        const char *slash = strchr(p, '/');
        uassert(13093, "bad --replSet config string format is: <setname>/<seedhost1>,<seedhost2>[,...]", slash != 0 && p != slash);
        _name = string(p, slash-p);
        log() << "replSet " << cfgString << endl;

        set<HostAndPort> temp;
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
                uassert(13096, "bad --replSet config string - dups?", temp.count(m) == 0 );
                temp.insert(m);
                uassert(13101, "can't use localhost in replset host list", !m.isLocalHost());
                if( m.isSelf() )
                    log() << "replSet ignoring seed " << m.toString() << " (=self)" << endl;
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

        loadConfig();
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
            startupStatusMsg = "loading admin.system.replset config (LOADINGCONFIG)";
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
                        startupStatusMsg = "can't get admin.system.replset config from self or any seed (uninitialized?)";
                        log() << "replSet can't get admin.system.replset config from self or any seed (EMPTYCONFIG)\n";
                        log() << "replSet have you ran replSetInitiate yet?\n";
                        log() << "replSet sleeping 1 minute and will try again." << endl;
                    }
                    else {
                        startupStatus = EMPTYUNREACHABLE;
                        startupStatusMsg = "can't currently get admin.system.replset config from self or any seed (EMPTYUNREACHABLE)";
                        log() << "replSet can't get admin.system.replset config from self or any seed.\n";
                        log() << "replSet sleeping 1 minute and will try again." << endl;
                    }

                    sleepsecs(60);
                    continue;
                }
                finishLoadingConfig(configs);
            }
            catch(AssertionException&) { 
                startupStatus = BADCONFIG;
                startupStatusMsg = "replSet error loading set config (BADCONFIG)";
                log() << "replSet error loading configurations\n";
                log() << "replSet replication will not start" << endl;
                fatal();
                throw;
            }
            break;
        }
        startupStatusMsg = "?";
        startupStatus = FINISHME;
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
            log() << "replSet Caught exception in management thread: " << e.what() << endl;
            if( theReplSet ) 
                theReplSet->fatal();
        }
    }

}
