/* connpool.cpp
*/

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

// _ todo: reconnect?

#include "stdafx.h"
#include "connpool.h"
#include "../db/commands.h"

namespace mongo {

    DBConnectionPool pool;
    
    DBClientBase* DBConnectionPool::get(const string& host) {
        boostlock L(poolMutex);
        
        PoolForHost *&p = pools[host];
        if ( p == 0 )
            p = new PoolForHost();
        if ( p->pool.empty() ) {
            string errmsg;
            DBClientBase *c;
            if( host.find(',') == string::npos ) {
                DBClientConnection *cc = new DBClientConnection(true);
                log(2) << "creating new connection for pool to:" << host << endl;
                if ( !cc->connect(host.c_str(), errmsg) ) {
                    delete cc;
                    uassert( (string)"dbconnectionpool: connect failed " + host , false);
                    return 0;
                }
                c = cc;
                onCreate( c );
            }
            else { 
                DBClientPaired *p = new DBClientPaired();
                if( !p->connect(host) ) { 
                    delete p;
                    uassert( (string)"dbconnectionpool: connect failed [2] " + host , false);
                    return 0;
                }
                c = p;
            }
            return c;
        }
        DBClientBase *c = p->pool.top();
        p->pool.pop();
        onHandedOut( c );
        return c;
    }

    void DBConnectionPool::flush(){
        boostlock L(poolMutex);
        for ( map<string,PoolForHost*>::iterator i = pools.begin(); i != pools.end(); i++ ){
            PoolForHost* p = i->second;

            vector<DBClientBase*> all;
            while ( ! p->pool.empty() ){
                DBClientBase * c = p->pool.top();
                p->pool.pop();
                all.push_back( c );
                bool res;
                c->isMaster( res );
            }
            
            for ( vector<DBClientBase*>::iterator i=all.begin(); i != all.end(); i++ ){
                p->pool.push( *i );
            }
        }
    }

    void DBConnectionPool::addHook( DBConnectionHook * hook ){
        _hooks.push_back( hook );
    }

    void DBConnectionPool::onCreate( DBClientBase * conn ){
        if ( _hooks.size() == 0 )
            return;
        
        for ( list<DBConnectionHook*>::iterator i = _hooks.begin(); i != _hooks.end(); i++ ){
            (*i)->onCreate( conn );
        }
    }

    void DBConnectionPool::onHandedOut( DBClientBase * conn ){
        if ( _hooks.size() == 0 )
            return;
        
        for ( list<DBConnectionHook*>::iterator i = _hooks.begin(); i != _hooks.end(); i++ ){
            (*i)->onHandedOut( conn );
        }
    }

    class PoolFlushCmd : public Command {
    public:
        PoolFlushCmd() : Command( "connpoolsync" ){}
        virtual bool run(const char*, mongo::BSONObj&, std::string&, mongo::BSONObjBuilder& result, bool){
            pool.flush();
            result << "ok" << 1;
            return true;
        }
        virtual bool slaveOk(){
            return true;
        }

    } poolFlushCmd;

} // namespace mongo
