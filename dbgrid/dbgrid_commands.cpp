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
     _ connection pool
     _ hostbyname_nonreentrant() problem
   _ gridconfig object which gets config from the grid db.
     connect to iad-sb-grid
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
#include "gridconfig.h"
#include "configserver.h"

namespace mongo {

    extern string ourHostname;
    
    namespace dbgrid_cmds {
        
        // --- internal commands ---

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
        
        class NetStatCmd : public GridAdminCmd {
        public:
            NetStatCmd() : GridAdminCmd("netstat") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                result.append("griddb", configServer.toString());
                result.append("isdbgrid", 1);
                return true;
            }
        } netstat;
        
        class ListDatabaseCommand : public GridAdminCmd {
        public:
            ListDatabaseCommand() : GridAdminCmd("listdatabases") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                // TODO
                result.append("not done", 1);
                return false;
            }
        } gridListDatabase;

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

        class ListServers : public GridAdminCmd {
        public:
            ListServers() : GridAdminCmd("listservers") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                DBClientWithCommands * conn = configServer.conn();

                vector<BSONObj> all;
                auto_ptr<DBClientCursor> cursor = conn->query( "grid.servers" , emptyObj );
                while ( cursor->more() ){
                    BSONObj o = cursor->next();
                    all.push_back( o );
                }
                
                result.append("servers" , all );
                result.append("ok" , 1 );
                return true;
            }
        } listServers;
        
        
        class AddServer : public GridAdminCmd {
        public:
            AddServer() : GridAdminCmd("addserver") { }
            bool run(const char *ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
                DBClientWithCommands * conn = configServer.conn();
                
                BSONObj server = BSON( "host" << cmdObj["addserver"].valuestrsafe() );
                
                BSONObj old = conn->findOne( "grid.servers" , server );
                if ( ! old.isEmpty() ){
                    result.append( "ok" , 0.0 );
                    result.append( "msg" , "already exists" );
                    return false;
                }
                
                conn->insert( "grid.servers" , server );
                result.append( "ok", 1 );
                result.append( "added" , server["host"].valuestrsafe() );
                return true;
            }
        } listMachines;
        
        
        // --- public commands ---

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
