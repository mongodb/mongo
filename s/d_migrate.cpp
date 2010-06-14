// d_migrate.cpp

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


/**
   these are commands that live in mongod
   mostly around shard management and checking
 */

#include "pch.h"
#include <map>
#include <string>

#include "../db/commands.h"
#include "../db/jsobj.h"
#include "../db/dbmessage.h"
#include "../db/query.h"

#include "../client/connpool.h"

#include "../util/queue.h"

#include "shard.h"
#include "d_logic.h"

using namespace std;

namespace mongo {


    class MoveShardStartCommand : public Command {
    public:
        MoveShardStartCommand() : Command( "movechunk.start" ){}
        virtual void help( stringstream& help ) const {
            help << "should not be calling this directly" << endl;
        }

        virtual bool slaveOk() const { return false; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return WRITE; } 
        
        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            // so i have to start clone, tell caller its ok to make change
            // at this point the caller locks me, and updates config db
            // then finish calls finish, and then deletes data when cursors are done
            
            string ns = cmdObj["movechunk.start"].valuestrsafe();
            string to = cmdObj["to"].valuestrsafe();
            string from = cmdObj["from"].valuestrsafe(); // my public address, a tad redundant, but safe
            BSONObj filter = cmdObj.getObjectField( "filter" );
            
            if ( ns.size() == 0 ){
                errmsg = "need to specify namespace in command";
                return false;
            }
            
            if ( to.size() == 0 ){
                errmsg = "need to specify server to move shard to";
                return false;
            }
            if ( from.size() == 0 ){
                errmsg = "need to specify server to move shard from (redundat i know)";
                return false;
            }
            
            if ( filter.isEmpty() ){
                errmsg = "need to specify a filter";
                return false;
            }
            
            log() << "got movechunk.start: " << cmdObj << endl;
            
            
            BSONObj res;
            bool ok;
            
            {
                dbtemprelease unlock;
                
                ScopedDbConnection conn( to );
                ok = conn->runCommand( "admin" , 
                                            BSON( "startCloneCollection" << ns <<
                                                  "from" << from <<
                                                  "query" << filter 
                                                  ) , 
                                            res );
                conn.done();
            }
            
            log() << "   movechunk.start res: " << res << endl;
            
            if ( ok ){
                result.append( res["finishToken"] );
            }
            else {
                errmsg = "startCloneCollection failed: ";
                errmsg += res["errmsg"].valuestrsafe();
            }
            return ok;
        }
        
    } moveShardStartCmd;

    class MoveShardFinishCommand : public Command {
    public:
        MoveShardFinishCommand() : Command( "movechunk.finish" ){}
        virtual void help( stringstream& help ) const {
            help << "should not be calling this directly" << endl;
        }

        virtual bool slaveOk() const { return false; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return WRITE; } 
        
        bool run(const string& , BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            // see MoveShardStartCommand::run
            
            string ns = cmdObj["movechunk.finish"].valuestrsafe();
            if ( ns.size() == 0 ){
                errmsg = "need ns as cmd value";
                return false;
            }

            string to = cmdObj["to"].valuestrsafe();
            if ( to.size() == 0 ){
                errmsg = "need to specify server to move shard to";
                return false;
            }


            unsigned long long newVersion = extractVersion( cmdObj["newVersion"] , errmsg );
            if ( newVersion == 0 ){
                errmsg = "have to specify new version number";
                return false;
            }
                                                        
            BSONObj finishToken = cmdObj.getObjectField( "finishToken" );
            if ( finishToken.isEmpty() ){
                errmsg = "need finishToken";
                return false;
            }
            
            if ( ns != finishToken["collection"].valuestrsafe() ){
                errmsg = "namespaced don't match";
                return false;
            }
            
            // now we're locked
            shardingState.setVersion( ns , newVersion );
            ShardedConnectionInfo::get(true)->setVersion( ns , newVersion );
            
            BSONObj res;
            bool ok;
            
            {
                dbtemprelease unlock;
                
                ScopedDbConnection conn( to );
                ok = conn->runCommand( "admin" , 
                                       BSON( "finishCloneCollection" << finishToken ) ,
                                       res );
                conn.done();
            }
            
            if ( ! ok ){
                // uh oh
                errmsg = "finishCloneCollection failed!";
                result << "finishError" << res;
                return false;
            }
            
            // wait until cursors are clean
            cout << "WARNING: deleting data before ensuring no more cursors TODO" << endl;
            
            {
                BSONObj removeFilter = finishToken.getObjectField( "query" );
                Client::Context ctx(ns);
                long long num = deleteObjects( ns.c_str() , removeFilter , false , true );
                log() << "movechunk.finish deleted: " << num << endl;
                result.appendNumber( "numDeleted" , num );
            }

            return true;
        }
        
    } moveShardFinishCmd;

}
