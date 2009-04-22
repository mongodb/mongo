// s/commands_public.cpp


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
#include "../util/message.h"
#include "../db/dbmessage.h"
#include "../client/connpool.h"
#include "../db/commands.h"

#include "config.h"
#include "shard.h"
#include "strategy.h"

namespace mongo {

    namespace dbgrid_pub_cmds {
        
        class PublicGridCommand : public Command {
        public:
            PublicGridCommand( const char * n ) : Command( n ){
            }
            virtual bool slaveOk(){
                return true;
            }
            virtual bool adminOnly() {
                return false;
            }
        };


        class CountCmd : public PublicGridCommand {
        public:
            CountCmd() : PublicGridCommand("count") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                
                string dbName = ns;
                dbName = dbName.substr( 0 , dbName.size() - 5 );
                string collection = cmdObj.firstElement().valuestrsafe();
                string fullns = dbName + "." + collection;
                
                BSONObj filter = cmdObj["query"].embeddedObject();

                DBConfig * conf = grid.getDBConfig( dbName , false );
                
                if ( ! conf || ! conf->isPartitioned() || ! conf->sharded( fullns ) ){
                    ScopedDbConnection conn( conf->getPrimary() );
                    result.append( "n" , (double)conn->count( fullns , filter ) );
                    conn.done();
                    result.append( "ok" , 1 );
                    return true;
                }
                
                ShardManager * sm = conf->getShardManager( fullns );
                massert( "how could shard manager be null!" , sm );
                
                vector<Shard*> shards;
                sm->getShardsForQuery( shards , filter );
                
                unsigned long long total = 0;
                for ( vector<Shard*>::iterator i = shards.begin() ; i != shards.end() ; i++ ){
                    Shard * s = *i;
                    total += s->countObjects();
                }
                
                result.append( "n" , (double)total );
                result.append( "ok" , 1 );
                return true;
            }
        } countCmd;
    }
}
