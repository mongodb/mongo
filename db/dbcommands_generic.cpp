// dbcommands_generic.cpp

/**
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
 * commands suited for any mongo server
 */

#include "pch.h"
#include "query.h"
#include "pdfile.h"
#include "jsobj.h"
#include "../bson/util/builder.h"
#include <time.h>
#include "introspect.h"
#include "btree.h"
#include "../util/lruishmap.h"
#include "../util/md5.hpp"
#include "../util/processinfo.h"
#include "json.h"
#include "repl.h"
#include "repl_block.h"
#include "replpair.h"
#include "commands.h"
#include "db.h"
#include "instance.h"
#include "lasterror.h"
#include "security.h"
#include "queryoptimizer.h"
#include "../scripting/engine.h"
#include "stats/counters.h"
#include "background.h"
#include "../util/version.h"

namespace mongo {

    class CmdBuildInfo : public Command {
    public:
        CmdBuildInfo() : Command( "buildInfo", true, "buildinfo" ) {}
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; } 
        virtual void help( stringstream &help ) const {
            help << "get version #, etc.\n";
            help << "{ buildinfo:1 }";
        }
        bool run(const string& dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            result << "version" << versionString << "gitVersion" << gitVersion() << "sysInfo" << sysInfo();
            result << "bits" << ( sizeof( int* ) == 4 ? 32 : 64 );
            result.appendBool( "debug" , debug );
            result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
            return true;
        }
    } cmdBuildInfo;

    class CmdGet : public Command {
    public:
        CmdGet() : Command( "get" ) { }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; } 
        virtual void help( stringstream &help ) const {
            help << "get administrative option(s)\nexample:\n";
            help << "{ get:1, notablescan:1 }\n";
            help << "supported so far:\n";
            help << "  quiet\n";
            help << "  notablescan\n";
            help << "  logLevel\n";
            help << "{ get:'*' } to get everything\n";
        }
        bool run(const string& dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            bool all = cmdObj.firstElement().valuestrsafe();
            
            int before = result.len();
            
            if( all || cmdObj.hasElement("quiet") ) {
                result.append("quiet", cmdLine.quiet );
            }
            if( all || cmdObj.hasElement("notablescan") ) {
                result.append("notablescan", cmdLine.noTableScan);
            }
            if( all || cmdObj.hasElement("logLevel") ) {
                result.append("logLevel", logLevel);
            }
            

            if ( before == result.len() ) {
                errmsg = "no option found to get";
                return false;
            }
            return true;
        }
    } cmdGet;

    class CmdSet : public Command {
    public:
        CmdSet() : Command( "set" ) { }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; } 
        virtual void help( stringstream &help ) const {
            help << "set administrative option(s)\nexample:\n";
            help << "{ set:1, notablescan:true }\n";
            help << "supported so far:\n";
            help << "  notablescan\n";
            help << "  logLevel\n";
            help << "  quiet\n";
        }
        bool run(const string& dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            
            if( cmdObj.hasElement("notablescan") ) {
                result.append("was", cmdLine.noTableScan);
                cmdLine.noTableScan = cmdObj["notablescan"].Bool();
            }
            else if( cmdObj.hasElement("quiet") ) {
                result.append("was", cmdLine.quiet );
                cmdLine.quiet = cmdObj["quiet"].Bool();
            }
            else if( cmdObj.hasElement( "logLevel" ) ) {
                result.append("was", logLevel );
                logLevel = cmdObj["logLevel"].numberInt();
            }
            else {
                errmsg = "no option found to set";
                return false;
            }
            return true;
        }
    } cmdSet;

    class PingCommand : public Command {
    public:
        PingCommand() : Command( "ping" ){}
        virtual bool slaveOk() const { return true; }
        virtual void help( stringstream &help ) const { help << "a way to check that the server is alive. responds immediately even if server is in a db lock."; }
        virtual LockType locktype() const { return NONE; }
        virtual bool requiresAuth() { return false; }
        virtual bool run(const string& badns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool){
            // IMPORTANT: Don't put anything in here that might lock db - including authentication
            return true;
        }
    } pingCmd;
    
    class FeaturesCmd : public Command {
    public:
        FeaturesCmd() : Command( "features", true ){}
        void help(stringstream& h) const { h << "return build level feature settings"; }
        virtual bool slaveOk() const { return true; }
        virtual bool readOnly(){ return true; }
        virtual LockType locktype() const { return READ; } 
        virtual bool run(const string& ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl){
            if ( globalScriptEngine ){
                BSONObjBuilder bb( result.subobjStart( "js" ) );
                result.append( "utf8" , globalScriptEngine->utf8Ok() );
                bb.done();
            }
            if ( cmdObj["oidReset"].trueValue() ){
                result.append( "oidMachineOld" , OID::staticMachine() );
                OID::newState();
            }
            result.append( "oidMachine" , OID::staticMachine() );
            return true;
        }
        
    } featuresCmd;

    class LogRotateCmd : public Command {
    public:
        LogRotateCmd() : Command( "logRotate" ){}
        virtual LockType locktype() const { return NONE; } 
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual bool run(const string& ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            rotateLogs();
            return 1;
        }        
        
    } logRotateCmd;
    
    class ListCommandsCmd : public Command {
    public:
        virtual void help( stringstream &help ) const { help << "get a list of all db commands"; }
        ListCommandsCmd() : Command( "listCommands", false ){}
        virtual LockType locktype() const { return NONE; } 
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return false; }
        virtual bool run(const string& ns, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            BSONObjBuilder b( result.subobjStart( "commands" ) );
            for ( map<string,Command*>::iterator i=_commands->begin(); i!=_commands->end(); ++i ){
                Command * c = i->second;

                // don't show oldnames
                if (i->first != c->name)
                    continue;

                BSONObjBuilder temp( b.subobjStart( c->name ) );

                {
                    stringstream help;
                    c->help( help );
                    temp.append( "help" , help.str() );
                }
                temp.append( "lockType" , c->locktype() );
                temp.append( "slaveOk" , c->slaveOk() );
                temp.append( "adminOnly" , c->adminOnly() );
                temp.done();
            }
            b.done();

            return 1;
        }        

    } listCommandsCmd;
    
    class CmdShutdown : public Command {
    public:
        virtual bool requiresAuth() { return true; }
        virtual bool adminOnly() const { return true; }
        virtual bool localHostOnlyIfNoAuth(const BSONObj& cmdObj) { return true; }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() const {
            return true;
        }
        virtual LockType locktype() const { return NONE; } 
        virtual void help( stringstream& help ) const {
            help << "shutdown the database.  must be ran against admin db and either (1) ran from localhost or (2) authenticated.\n";
        }
        CmdShutdown() : Command("shutdown") {}
        bool run(const string& dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            Client * c = currentClient.get();
            if ( c ) {
                c->shutdown();
            }
            
            log() << "terminating, shutdown command received" << endl;

            dbexit( EXIT_CLEAN , "shutdown called" , true ); // this never returns
            assert(0);
            return true;
        }
    } cmdShutdown;

    /* for testing purposes only */
    class CmdForceError : public Command {
    public:
        virtual void help( stringstream& help ) const {
            help << "for testing purposes only.  forces a user assertion exception";
        }
        virtual bool logTheOp() {
            return false;
        }
        virtual bool slaveOk() const {
            return true;
        }
        virtual LockType locktype() const { return NONE; } 
        CmdForceError() : Command("forceerror") {}
        bool run(const string& dbnamne, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            uassert( 10038 , "forced error", false);
            return true;
        }
    } cmdForceError;

}
