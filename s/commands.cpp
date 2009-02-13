// dbgrid/request.cpp

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

/* TODO
   _ concurrency control.
   _ limit() works right?
   _ KillCursors

   later
   _ secondary indexes
*/

#include "stdafx.h"
#include "../util/message.h"
#include "../db/dbmessage.h"
#include "../client/connpool.h"
#include "../db/commands.h"
#include "config.h"

namespace mongo {

    extern string ourHostname;
    
    namespace dbgrid_cmds {
        
        set<string> dbgridCommands;

        class GridAdminCmd : public Command {
        public:
            GridAdminCmd( const char * n ) : Command( n ){
                dbgridCommands.insert( n );
            }
            virtual bool slaveOk(){
                return true;
            }
            virtual bool adminOnly() {
                return true;
            }
        };
        
        // --------------- misc commands ----------------------

        class NetStatCmd : public GridAdminCmd {
        public:
            NetStatCmd() : GridAdminCmd("netstat") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                result.append("configserver", configServer.getPrimary() );
                result.append("isdbgrid", 1);
                return true;
            }
        } netstat;

        class ListGridCommands : public GridAdminCmd {
        public:
            ListGridCommands() : GridAdminCmd("gridcommands") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                
                BSONObjBuilder arr;
                int num=0;
                for ( set<string>::iterator i = dbgridCommands.begin(); i != dbgridCommands.end(); i++ ){
                    string s = BSONObjBuilder::numStr( num++ );
                    arr.append( s.c_str() , *i );
                }
                
                result.appendArray( "commands" , arr.done() );
                result.append("ok" , 1 );
                return true;
            }
        } listGridCommands;        


        // ------------ database level commands -------------
        
        class ListDatabaseCommand : public GridAdminCmd {
        public:
            ListDatabaseCommand() : GridAdminCmd("listdatabases") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                ScopedDbConnection conn( configServer.getPrimary() );                
        

                auto_ptr<DBClientCursor> cursor = conn->query( "config.databases" , emptyObj );

                BSONObjBuilder list;                
                int num = 0;
                while ( cursor->more() ){
                    string s = BSONObjBuilder::numStr( num++ );
                    
                    BSONObj o = cursor->next();
                    list.append( s.c_str() , o["name"].valuestrsafe() );
                }
                
                result.appendArray("databases" , list.obj() );
                conn.done();

                return true;        
            }
        } gridListDatabase;

        // ------------ server level commands -------------

        class ListServers : public GridAdminCmd {
        public:
            ListServers() : GridAdminCmd("listservers") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                ScopedDbConnection conn( configServer.getPrimary() );

                vector<BSONObj> all;
                auto_ptr<DBClientCursor> cursor = conn->query( "config.servers" , emptyObj );
                while ( cursor->more() ){
                    BSONObj o = cursor->next();
                    all.push_back( o );
                }
                
                result.append("servers" , all );
                result.append("ok" , 1 );
                conn.done();

                return true;
            }
        } listServers;
        
        
        class AddServer : public GridAdminCmd {
        public:
            AddServer() : GridAdminCmd("addserver") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                ScopedDbConnection conn( configServer.getPrimary() );
                
                BSONObj server = BSON( "host" << cmdObj["addserver"].valuestrsafe() );
                
                BSONObj old = conn->findOne( "config.servers" , server );
                if ( ! old.isEmpty() ){
                    result.append( "ok" , 0.0 );
                    result.append( "msg" , "already exists" );
                    conn.done();
                    return false;
                }
                
                conn->insert( "config.servers" , server );
                result.append( "ok", 1 );
                result.append( "added" , server["host"].valuestrsafe() );
                conn.done();
                return true;
            }
        } addServer;
        
        class RemoveServer : public GridAdminCmd {
        public:
            RemoveServer() : GridAdminCmd("removeserver") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                ScopedDbConnection conn( configServer.getPrimary() );
                
                BSONObj server = BSON( "host" << cmdObj["removeserver"].valuestrsafe() );
                conn->remove( "config.servers" , server );
                
                conn.done();
                return true;
            }
        } removeServer;

        
        // --------------- public commands ----------------

        class IsDbGridCmd : public Command {
        public:
            virtual bool slaveOk() {
                return true;
            }
            IsDbGridCmd() : Command("isdbgrid") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                result.append("isdbgrid", 1);
                result.append("hostname", ourHostname);
                return true;
            }
        } isdbgrid;

        class CmdIsMaster : public Command {
        public:
            virtual bool requiresAuth() { return false; }
            virtual bool slaveOk() {
                return true;
            }
            CmdIsMaster() : Command("ismaster") { }
            virtual bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool) {
                result.append("ismaster", 0.0);
                result.append("msg", "isdbgrid");
                return true;
            }
        } ismaster;

    }

} // namespace mongo
