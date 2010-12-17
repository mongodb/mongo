// dbclient.cpp - connect to a Mongo database as a database, from C++

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"
#include "../db/pdfile.h"
#include "dbclient.h"
#include "../bson/util/builder.h"
#include "../db/jsobj.h"
#include "../db/json.h"
#include "../db/instance.h"
#include "../util/md5.hpp"
#include "../db/dbmessage.h"
#include "../db/cmdline.h"
#include "connpool.h"
#include "../s/util.h"

namespace mongo {

    DBClientReplicaSet::DBClientReplicaSet( const string& name , const vector<HostAndPort>& servers )
        : _name( name ) , _currentMaster( 0 ), _servers( servers ){
        
        for ( unsigned i=0; i<_servers.size(); i++ )
            _conns.push_back( new DBClientConnection( true , this ) );
    }
    
    DBClientReplicaSet::~DBClientReplicaSet(){
        for ( unsigned i=0; i<_conns.size(); i++ )
            delete _conns[i];
        _conns.clear();
    }

    string DBClientReplicaSet::toString() {
        return getServerAddress();
    }

    string DBClientReplicaSet::getServerAddress() const {
        StringBuilder ss;
        if ( _name.size() )
            ss << _name << "/";
    
        for ( unsigned i=0; i<_servers.size(); i++ ){
            if ( i > 0 )
                ss << ",";
            ss << _servers[i].toString();
        }
        return ss.str();
    }

    /* find which server currently primary */
    void DBClientReplicaSet::_checkMaster() {
        
        bool triedQuickCheck = false;
        
        log( _logLevel + 1) <<  "_checkMaster on: " << toString() << endl;
        for ( int retry = 0; retry < 2; retry++ ) {
            for ( unsigned i=0; i<_conns.size(); i++ ){
                DBClientConnection * c = _conns[i];
                try {
                    bool im;
                    BSONObj o;
                    c->isMaster(im, &o);
                    
                    if ( retry )
                        log(_logLevel) << "checkmaster: " << c->toString() << ' ' << o << '\n';
                    
                    string maybePrimary;
                    if ( o["hosts"].type() == Array ){
                        if ( o["primary"].type() == String )
                            maybePrimary = o["primary"].String();
                        
                        BSONObjIterator hi(o["hosts"].Obj());
                        while ( hi.more() ){
                            string toCheck = hi.next().String();
                            int found = -1;
                            for ( unsigned x=0; x<_servers.size(); x++ ){
                                if ( toCheck == _servers[x].toString() ){
                                    found = x;
                                    break;
                                }
                            }
                            
                            if ( found == -1 ){
                                HostAndPort h( toCheck );
                                _servers.push_back( h );
                                _conns.push_back( new DBClientConnection( true, this ) );
                                string temp;
                                _conns[ _conns.size() - 1 ]->connect( h , temp );
                                log( _logLevel ) << "updated set to: " << toString() << endl;
                            }
                            
                        }
                    }

                    if ( im ) {
                        _currentMaster = c;
                        return;
                    }
                    
                    if ( maybePrimary.size() && ! triedQuickCheck ){
                        for ( unsigned x=0; x<_servers.size(); x++ ){
                            if ( _servers[i].toString() != maybePrimary )
                                continue;
                            triedQuickCheck = true;
                            _conns[x]->isMaster( im , &o );
                            if ( im ){
                                _currentMaster = _conns[x];
                                return;
                            }
                        }
                    }
                }
                catch ( std::exception& e ) {
                    if ( retry )
                        log(_logLevel) << "checkmaster: caught exception " << c->toString() << ' ' << e.what() << endl;
                }
            }
            sleepsecs(1);
        }

        uassert( 10009 , "checkmaster: no master found", false);
    }

    DBClientConnection * DBClientReplicaSet::checkMaster() {
        if ( _currentMaster ){
            // a master is selected.  let's just make sure connection didn't die
            if ( ! _currentMaster->isFailed() )
                return _currentMaster;
            _currentMaster = 0;
        }

        _checkMaster();
        assert( _currentMaster );
        return _currentMaster;
    }

    DBClientConnection& DBClientReplicaSet::masterConn(){
        return *checkMaster();
    }

    DBClientConnection& DBClientReplicaSet::slaveConn(){
        DBClientConnection * m = checkMaster();
        assert( ! m->isFailed() );
        
        DBClientConnection * failedSlave = 0;

        for ( unsigned i=0; i<_conns.size(); i++ ){
            if ( m == _conns[i] )
                continue;
            failedSlave = _conns[i];
            if ( _conns[i]->isFailed() )
                continue;
            return *_conns[i];
        }

        assert(failedSlave);
        return *failedSlave;
    }

    bool DBClientReplicaSet::connect(){
        string errmsg;

        bool anyGood = false;
        for ( unsigned i=0; i<_conns.size(); i++ ){
            if ( _conns[i]->connect( _servers[i] , errmsg ) )
                anyGood = true;
        }
        
        if ( ! anyGood )
            return false;

        try {
            checkMaster();
        }
        catch (AssertionException&) {
            return false;
        }
        return true;
    }

	bool DBClientReplicaSet::auth(const string &dbname, const string &username, const string &pwd, string& errmsg, bool digestPassword ) { 
		DBClientConnection * m = checkMaster();
		if( !m->auth(dbname, username, pwd, errmsg, digestPassword ) )
			return false;
        
		/* we try to authentiate with the other half of the pair -- even if down, that way the authInfo is cached. */
        for ( unsigned i=0; i<_conns.size(); i++ ){
            if ( _conns[i] == m )
                continue;
            try {
                string e;
                _conns[i]->auth( dbname , username , pwd , e , digestPassword );
            }
            catch ( AssertionException& ){
            }
        }

		return true;
	}

    auto_ptr<DBClientCursor> DBClientReplicaSet::query(const string &a, Query b, int c, int d,
                                                   const BSONObj *e, int f, int g){
        // TODO: if slave ok is set go to a slave
        return checkMaster()->query(a,b,c,d,e,f,g);
    }

    BSONObj DBClientReplicaSet::findOne(const string &a, const Query& b, const BSONObj *c, int d) {
        return checkMaster()->findOne(a,b,c,d);
    }

    bool DBClientReplicaSet::isMember( const DBConnector * conn ) const {
        if ( conn == this )
            return true;
        
        for ( unsigned i=0; i<_conns.size(); i++ )
            if ( _conns[i]->isMember( conn ) )
                return true;
        
        return false;
    }
    


}
