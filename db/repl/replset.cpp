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

#include "stdafx.h"
#include "../cmdline.h"
#include "../../util/sock.h"
#include "replset.h"
#include "rs_config.h"

namespace mongo { 

    ReplSet *theReplSet = 0;

    void ReplSet::fillIsMaster(BSONObjBuilder& b) {
        b.append("ismaster", 0);
        b.append("ok", false);
        b.append("msg", "not yet implemented");
    }

    /** @param cfgString <setname>/<seedhost1>,<seedhost2> */
    ReplSet::ReplSet(string cfgString) : fatal(false) {
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

        startHealthThreads();
    }

    string ReplSet::startupStatusMsg;

    void ReplSet::loadConfig() {
        startupStatusMsg = "loading admin.replset config";
        while( 1 ) {
            try {
                vector<ReplSetConfig> configs;
                configs.push_back( ReplSetConfig(HostAndPort::me()) );
                for( vector<HostAndPort>::const_iterator i = _seeds->begin(); i != _seeds->end(); i++ )
                    configs.push_back( ReplSetConfig(*i) );
                int nok = 0;
                for( vector<ReplSetConfig>::iterator i = configs.begin(); i != configs.end(); i++ ) { 
                    if( i->ok() )
                        nok++;
                }
                if( nok == 0 ) { 
                    startupStatusMsg = "can't currently get admin.replset config from self or any seed";
                    log() << "replSet can't get admin.replset config from self or any seed.\n";
                    log() << "replSet sleeping 1 minute and will try again." << endl;
                    sleepsecs(60);
                    continue;
                }
            }
            catch(AssertionException&) { 
                log() << "replSet error loading configurations. admin.replset may be misconfigured\n";
                log() << "replSet replication will not start" << endl;
                fatal = true;
                throw;
            }
            break;
        }
        startupStatusMsg = "?";
    }

    /*void ReplSet::addMemberIfMissing(const HostAndPort& h) { 
        MemberInfo *m = _members.head();
        while( m ) {
            if( h.host() == m->host && h.port() == m->port )
                return;
            m = m->next();
        }
        MemberInfo *nm = new MemberInfo(h.host(), h.port());
        _members.push(nm);
    }*/

    /* called at initialization */
    void startReplSets() {
        mongo::lastError.reset( new LastError() );
        try { 
            assert( theReplSet == 0 );
            if( cmdLine.replSet.empty() )
                return;
            theReplSet = new ReplSet(cmdLine.replSet);
        }
        catch(...) { 
            log() << "Caught exception in repl set management thread" << endl;
            if( theReplSet ) 
                theReplSet->fatal = true;
        }
    }

}
