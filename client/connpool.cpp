/* connpool.cpp
*/

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
                    uassert( (string)"dbconnectionpool: connect failed" + host , false);
                    return 0;
                }
                c = cc;
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
