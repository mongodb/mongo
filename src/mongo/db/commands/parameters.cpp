// parameters.cpp

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

#include "mongo/client/dbclient_rs.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/commands.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    namespace {
        void appendParameterNames( stringstream& help ) {
            help << "supported:\n";
            const ServerParameter::Map& m = ServerParameterSet::getGlobal()->getMap();
            for ( ServerParameter::Map::const_iterator i = m.begin(); i != m.end(); ++i ) {
                help << "  " << i->first << "\n";
            }
        }
    }

    class CmdGet : public Command {
    public:
        CmdGet() : Command( "getParameter" ) { }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::getParameter);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        virtual void help( stringstream &help ) const {
            help << "get administrative option(s)\nexample:\n";
            help << "{ getParameter:1, notablescan:1 }\n";
            appendParameterNames( help );
            help << "{ getParameter:'*' } to get everything\n";
        }
        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            bool all = *cmdObj.firstElement().valuestrsafe() == '*';

            int before = result.len();

            // TODO: convert to ServerParameters -- SERVER-10515

            if( cmdLine.dur && (all || cmdObj.hasElement("journalCommitInterval")) ) {
                result.append("journalCommitInterval",
                              cmdLine.journalCommitInterval);
            }
            if( all || cmdObj.hasElement( "traceExceptions" ) ) {
                result.append("traceExceptions",
                              DBException::traceExceptions);
            }
            if( all || cmdObj.hasElement( "replMonitorMaxFailedChecks" ) ) {
                result.append("replMonitorMaxFailedChecks",
                              ReplicaSetMonitor::getMaxFailedChecks());
            }

            const ServerParameter::Map& m = ServerParameterSet::getGlobal()->getMap();
            for ( ServerParameter::Map::const_iterator i = m.begin(); i != m.end(); ++i ) {
                if ( all || cmdObj.hasElement( i->first.c_str() ) ) {
                    i->second->append( result, i->second->name() );
                }
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
        CmdSet() : Command( "setParameter" ) { }
        virtual bool slaveOk() const { return true; }
        virtual bool adminOnly() const { return true; }
        virtual LockType locktype() const { return NONE; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::setParameter);
            out->push_back(Privilege(AuthorizationManager::SERVER_RESOURCE_NAME, actions));
        }
        virtual void help( stringstream &help ) const {
            help << "set administrative option(s)\n";
            help << "{ setParameter:1, <param>:<value> }\n";
            appendParameterNames( help );
        }
        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            int s = 0;
            bool found = false;

            // TODO: convert to ServerParameters -- SERVER-10515

            if( cmdObj.hasElement("journalCommitInterval") ) {
                if( !cmdLine.dur ) {
                    errmsg = "journaling is off";
                    return false;
                }
                int x = (int) cmdObj["journalCommitInterval"].Number();
                verify( x > 1 && x < 500 );
                cmdLine.journalCommitInterval = x;
                log() << "setParameter journalCommitInterval=" << x << endl;
                s++;
            }
            if( cmdObj.hasElement( "traceExceptions" ) ) {
                if( s == 0 ) result.append( "was", DBException::traceExceptions );
                DBException::traceExceptions = cmdObj["traceExceptions"].Bool();
                s++;
            }
            if( cmdObj.hasElement( "replMonitorMaxFailedChecks" ) ) {
                if( s == 0 ) result.append( "was", ReplicaSetMonitor::getMaxFailedChecks() );
                ReplicaSetMonitor::setMaxFailedChecks(
                        cmdObj["replMonitorMaxFailedChecks"].numberInt() );
                s++;
            }

            const ServerParameter::Map& m = ServerParameterSet::getGlobal()->getMap();
            BSONObjIterator i( cmdObj );
            i.next(); // skip past command name
            while ( i.more() ) {
                BSONElement e = i.next();
                ServerParameter::Map::const_iterator j = m.find( e.fieldName() );
                if ( j == m.end() )
                    continue;

                if ( ! j->second->allowedToChangeAtRuntime() ) {
                    errmsg = str::stream()
                        << "not allowed to change ["
                        << e.fieldName()
                        << "] at runtime";
                    return false;
                }

                if ( s == 0 )
                    j->second->append( result, "was" );

                Status status = j->second->set( e );
                if ( status.isOK() ) {
                    s++;
                    continue;
                }
                errmsg = status.reason();
                result.append( "code", status.code() );
                return false;
            }

            if( s == 0 && !found ) {
                errmsg = "no option found to set, use help:true to see options ";
                return false;
            }

            return true;
        }
    } cmdSet;

    namespace {
        class LogLevelSetting : public ServerParameter {
        public:
            LogLevelSetting() : ServerParameter(ServerParameterSet::getGlobal(), "logLevel") {}

            virtual void append(BSONObjBuilder& b, const std::string& name) {
                b << name << logger::globalLogDomain()->getMinimumLogSeverity().toInt();
            }

            virtual Status set(const BSONElement& newValueElement) {
                typedef logger::LogSeverity LogSeverity;
                int newValue;
                if (!newValueElement.coerce(&newValue) || newValue < 0)
                    return Status(ErrorCodes::BadValue, mongoutils::str::stream() <<
                                  "Invalid value for logLevel: " << newValueElement);
                LogSeverity newSeverity = (newValue > 0) ? LogSeverity::Debug(newValue) :
                    LogSeverity::Log();
                logger::globalLogDomain()->setMinimumLoggedSeverity(newSeverity);
                return Status::OK();
            }

            virtual Status setFromString(const std::string& str) {
                typedef logger::LogSeverity LogSeverity;
                int newValue;
                Status status = parseNumberFromString(str, &newValue);
                if (!status.isOK())
                    return status;
                if (newValue < 0)
                    return Status(ErrorCodes::BadValue, mongoutils::str::stream() <<
                                  "Invalid value for logLevel: " << newValue);
                LogSeverity newSeverity = (newValue > 0) ? LogSeverity::Debug(newValue) :
                    LogSeverity::Log();
                logger::globalLogDomain()->setMinimumLoggedSeverity(newSeverity);
                return Status::OK();
            }
        } logLevelSetting;

        ExportedServerParameter<bool> NoTableScanSetting( ServerParameterSet::getGlobal(),
                                                          "notablescan",
                                                          &cmdLine.noTableScan,
                                                          true,
                                                          true );

        ExportedServerParameter<bool> QuietSetting( ServerParameterSet::getGlobal(),
                                                    "quiet",
                                                    &cmdLine.quiet,
                                                    true,
                                                    true );

        ExportedServerParameter<double> SyncdelaySetting( ServerParameterSet::getGlobal(),
                                                          "syncdelay",
                                                          &cmdLine.syncdelay,
                                                          true,
                                                          true );
    }

}

