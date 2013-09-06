/** @file dbcommands_generic.cpp commands suited for any mongo server (both mongod, mongos) */

/**
*    Copyright (C) 2012 10gen Inc.
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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/pch.h"

#include <time.h>

#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/background.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/shutdown.h"
#include "mongo/db/db.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/repl/multicmd.h"
#include "mongo/db/repl/write_concern.h"
#include "mongo/server.h"
#include "mongo/db/stats/counters.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/lruishmap.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/processinfo.h"
#include "mongo/util/ramlog.h"
#include "mongo/util/version.h"

namespace mongo {

#if 0
    namespace cloud {
        SimpleMutex mtx("cloud");
        Guarded< vector<string>, mtx > ips;
        bool startedThread = false;

        void thread() { 
            bson::bo cmd;
            while( 1 ) {
                list<Target> L;
                {
                    SimpleMutex::scoped_lock lk(mtx);
                    if( ips.ref(lk).empty() )
                        continue;
                    for( unsigned i = 0; i < ips.ref(lk).size(); i++ ) { 
                        L.push_back( Target(ips.ref(lk)[i]) );
                    }
                }


                /** repoll as machines might be down on the first lookup (only if not found previously) */
                sleepsecs(6); 
            }
        }
    }

    class CmdCloud : public Command {
    public:
        CmdCloud() : Command( "cloud" ) { }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; }
        virtual void help( stringstream &help ) const {
            help << "internal command facilitating running in certain cloud computing environments";
        }
        bool run(const string& dbname, BSONObj& obj, int options, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            if( !obj.hasElement("servers") ) { 
                vector<string> ips;
                obj["servers"].Obj().Vals(ips);
                {
                    SimpleMutex::scoped_lock lk(cloud::mtx);
                    cloud::ips.ref(lk).swap(ips);
                    if( !cloud::startedThread ) {
                        cloud::startedThread = true;
                        boost::thread thr(cloud::thread);
                    }
                }
            }
            return true;
        }
    } cmdCloud;
#endif

    class CmdBuildInfo : public Command {
    public:
        CmdBuildInfo() : Command( "buildInfo", true, "buildinfo" ) {}
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return false; }
        virtual LockType locktype() const { return NONE; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        virtual void help( stringstream &help ) const {
            help << "get version #, etc.\n";
            help << "{ buildinfo:1 }";
        }

        bool run(const std::string& dbname,
                 BSONObj& jsobj,
                 int, // options
                 std::string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl) {
        appendBuildInfo(result);
        return true;

        }

    } cmdBuildInfo;


    class PingCommand : public Command {
    public:
        PingCommand() : Command( "ping" ) {}
        virtual bool slaveOk() const { return true; }
        virtual void help( stringstream &help ) const { help << "a way to check that the server is alive. responds immediately even if server is in a db lock."; }
        virtual LockType locktype() const { return NONE; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        virtual bool run(const string& badns, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            // IMPORTANT: Don't put anything in here that might lock db - including authentication
            return true;
        }
    } pingCmd;

    class FeaturesCmd : public Command {
    public:
        FeaturesCmd() : Command( "features", true ) {}
        void help(stringstream& h) const { h << "return build level feature settings"; }
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return NONE; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        virtual bool run(const string& ns, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            if ( globalScriptEngine ) {
                BSONObjBuilder bb( result.subobjStart( "js" ) );
                result.append( "utf8" , globalScriptEngine->utf8Ok() );
                bb.done();
            }
            if ( cmdObj["oidReset"].trueValue() ) {
                result.append( "oidMachineOld" , OID::getMachineId() );
                OID::regenMachineId();
            }
            result.append( "oidMachine" , OID::getMachineId() );
            return true;
        }

    } featuresCmd;

    class HostInfoCmd : public Command {
    public:
        HostInfoCmd() : Command("hostInfo", true) {}
        virtual bool slaveOk() const {
            return true;
        }

        virtual LockType locktype() const { return NONE; }

        virtual void help( stringstream& help ) const {
            help << "returns information about the daemon's host";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::hostInfo);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            ProcessInfo p;
            BSONObjBuilder bSys, bOs;

            bSys.appendDate( "currentTime" , jsTime() );
            bSys.append( "hostname" , prettyHostName() );
            bSys.append( "cpuAddrSize", p.getAddrSize() );
            bSys.append( "memSizeMB", static_cast <unsigned>( p.getMemSizeMB() ) );
            bSys.append( "numCores", p.getNumCores() );
            bSys.append( "cpuArch", p.getArch() );
            bSys.append( "numaEnabled", p.hasNumaEnabled() );
            bOs.append( "type", p.getOsType() );
            bOs.append( "name", p.getOsName() );
            bOs.append( "version", p.getOsVersion() );

            result.append( StringData( "system" ), bSys.obj() );
            result.append( StringData( "os" ), bOs.obj() );
            p.appendSystemDetails( result );

            return true;
        }

    } hostInfoCmd;

    class LogRotateCmd : public Command {
    public:
        LogRotateCmd() : Command( "logRotate" ) {}
        virtual LockType locktype() const { return NONE; }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::logRotate);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        virtual bool run(const string& ns, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            return rotateLogs();
        }

    } logRotateCmd;

    class ListCommandsCmd : public Command {
    public:
        virtual void help( stringstream &help ) const { help << "get a list of all db commands"; }
        ListCommandsCmd() : Command( "listCommands", false ) {}
        virtual LockType locktype() const { return NONE; }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return false; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        virtual bool run(const string& ns, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            BSONObjBuilder b( result.subobjStart( "commands" ) );
            for ( map<string,Command*>::iterator i=_commands->begin(); i!=_commands->end(); ++i ) {
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
                //optionally indicates that the command can be forced to run on a slave/secondary
                if ( c->slaveOverrideOk() ) temp.append( "slaveOverrideOk" , c->slaveOverrideOk() );
                temp.done();
            }
            b.done();

            return 1;
        }

    } listCommandsCmd;

    bool CmdShutdown::shutdownHelper() {
        Client * c = currentClient.get();
        if ( c ) {
            c->shutdown();
        }

        log() << "terminating, shutdown command received" << endl;

        dbexit( EXIT_CLEAN , "shutdown called" ); // this never returns
        verify(0);
        return true;
    }

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
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        CmdForceError() : Command("forceerror") {}
        bool run(const string& dbnamne, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            uassert( 10038 , "forced error", false);
            return true;
        }
    } cmdForceError;

    class AvailableQueryOptions : public Command {
    public:
        AvailableQueryOptions() : Command( "availableQueryOptions" , false , "availablequeryoptions" ) {}
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return NONE; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {} // No auth required
        virtual bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            result << "options" << QueryOption_AllSupported;
            return true;
        }
    } availableQueryOptionsCmd;

    class GetLogCmd : public Command {
    public:
        GetLogCmd() : Command( "getLog" ){}

        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return NONE; }
        virtual bool adminOnly() const { return true; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::getLog);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        virtual void help( stringstream& help ) const {
            help << "{ getLog : '*' }  OR { getLog : 'global' }";
        }

        virtual bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            string p = cmdObj.firstElement().String();
            if ( p == "*" ) {
                vector<string> names;
                RamLog::getNames( names );

                BSONArrayBuilder arr;
                for ( unsigned i=0; i<names.size(); i++ ) {
                    arr.append( names[i] );
                }
                
                result.appendArray( "names" , arr.arr() );
            }
            else {
                RamLog* ramlog = RamLog::getIfExists(p);
                if ( ! ramlog ) {
                    errmsg = str::stream() << "no RamLog named: " << p;
                    return false;
                }
                RamLog::LineIterator rl(ramlog);

                result.appendNumber( "totalLinesWritten", rl.getTotalLinesWritten() );

                BSONArrayBuilder arr( result.subarrayStart( "log" ) );
                while (rl.more())
                    arr.append(rl.next());
                arr.done();
            }
            return true;
        }

    } getLogCmd;

    class CmdGetCmdLineOpts : Command {
    public:
        CmdGetCmdLineOpts(): Command("getCmdLineOpts") {}
        void help(stringstream& h) const { h << "get argv"; }
        virtual LockType locktype() const { return NONE; }
        virtual bool adminOnly() const { return true; }
        virtual bool slaveOk() const { return true; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::getCmdLineOpts);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        virtual bool run(const string&, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            result.append("argv", CmdLine::getArgvArray());
            result.append("parsed", CmdLine::getParsedOpts());
            return true;
        }

    } cmdGetCmdLineOpts;

}
