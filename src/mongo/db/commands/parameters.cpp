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

#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/security_key.h"
#include "mongo/db/commands.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage_options.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"

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
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::getParameter);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }
        virtual void help( stringstream &help ) const {
            help << "get administrative option(s)\nexample:\n";
            help << "{ getParameter:1, notablescan:1 }\n";
            appendParameterNames( help );
            help << "{ getParameter:'*' } to get everything\n";
        }
        bool run(OperationContext* txn, const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            bool all = *cmdObj.firstElement().valuestrsafe() == '*';

            int before = result.len();

            const ServerParameter::Map& m = ServerParameterSet::getGlobal()->getMap();
            for ( ServerParameter::Map::const_iterator i = m.begin(); i != m.end(); ++i ) {
                if ( all || cmdObj.hasElement( i->first.c_str() ) ) {
                    i->second->append(txn, result, i->second->name() );
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
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::setParameter);
            out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
        }
        virtual void help( stringstream &help ) const {
            help << "set administrative option(s)\n";
            help << "{ setParameter:1, <param>:<value> }\n";
            appendParameterNames( help );
        }
        bool run(OperationContext* txn, const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            int s = 0;
            bool found = false;

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
                    j->second->append(txn, result, "was" );

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

            virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name) {
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

        class SSLModeSetting : public ServerParameter {
        public:
            SSLModeSetting() : ServerParameter(ServerParameterSet::getGlobal(), "sslMode",
                                               false, // allowedToChangeAtStartup
                                               true // allowedToChangeAtRuntime
                                              ) {}

            std::string sslModeStr() {
                switch (sslGlobalParams.sslMode.load()) {
                    case SSLGlobalParams::SSLMode_disabled:
                        return "disabled";
                    case SSLGlobalParams::SSLMode_allowSSL:
                        return "allowSSL";
                    case SSLGlobalParams::SSLMode_preferSSL:
                        return "preferSSL";
                    case SSLGlobalParams::SSLMode_requireSSL:
                        return "requireSSL";
                    default:
                        return "undefined";
                }
            }

            virtual void append(
                            OperationContext* txn, BSONObjBuilder& b, const std::string& name) {
                b << name << sslModeStr();
            }

            virtual Status set(const BSONElement& newValueElement) {
                try {
                    return setFromString(newValueElement.String());
                }
                catch (MsgAssertionException msg) {
                    return Status(ErrorCodes::BadValue, mongoutils::str::stream() <<
                                    "Invalid value for sslMode via setParameter command: " 
                                    << newValueElement);
                }

            }

            virtual Status setFromString(const std::string& str) {
#ifndef MONGO_SSL
                return Status(ErrorCodes::IllegalOperation, mongoutils::str::stream() <<
                                "Unable to set sslMode, SSL support is not compiled into server");
#endif
                if (str != "disabled" && str != "allowSSL" &&
                    str != "preferSSL" && str != "requireSSL") { 
                        return Status(ErrorCodes::BadValue, mongoutils::str::stream() <<
                                    "Invalid value for sslMode via setParameter command: " 
                                    << str);
                }

                int oldMode = sslGlobalParams.sslMode.load();
                if (str == "preferSSL" && oldMode == SSLGlobalParams::SSLMode_allowSSL) {
                    sslGlobalParams.sslMode.store(SSLGlobalParams::SSLMode_preferSSL);
                }
                else if (str == "requireSSL" && oldMode == SSLGlobalParams::SSLMode_preferSSL) {
                    sslGlobalParams.sslMode.store(SSLGlobalParams::SSLMode_requireSSL);
                }
                else {
                    return Status(ErrorCodes::BadValue, mongoutils::str::stream() <<
                                    "Illegal state transition for sslMode, attempt to change from "
                                    << sslModeStr() << " to " << str);
                }
                return Status::OK();
            }
        } sslModeSetting;
        
        class ClusterAuthModeSetting : public ServerParameter {
        public:
            ClusterAuthModeSetting() : 
                ServerParameter(ServerParameterSet::getGlobal(), "clusterAuthMode",
                                false, // allowedToChangeAtStartup
                                true // allowedToChangeAtRuntime
                               ) {}

            std::string clusterAuthModeStr() {
                switch (serverGlobalParams.clusterAuthMode.load()) {
                    case ServerGlobalParams::ClusterAuthMode_keyFile:
                        return "keyFile";
                    case ServerGlobalParams::ClusterAuthMode_sendKeyFile:
                        return "sendKeyFile";
                    case ServerGlobalParams::ClusterAuthMode_sendX509:
                        return "sendX509";
                    case ServerGlobalParams::ClusterAuthMode_x509:
                        return "x509";
                    default:
                        return "undefined";
                }
            }

            virtual void append(
                            OperationContext* txn, BSONObjBuilder& b, const std::string& name) {
                b << name << clusterAuthModeStr();
            }

            virtual Status set(const BSONElement& newValueElement) {
                try {
                    return setFromString(newValueElement.String());
                }
                catch (MsgAssertionException msg) {
                    return Status(ErrorCodes::BadValue, mongoutils::str::stream() <<
                                    "Invalid value for clusterAuthMode via setParameter command: " 
                                    << newValueElement);
                }

            }

            virtual Status setFromString(const std::string& str) {
#ifndef MONGO_SSL
                return Status(ErrorCodes::IllegalOperation, mongoutils::str::stream() <<
                                "Unable to set clusterAuthMode, " <<
                                "SSL support is not compiled into server");
#endif
                if (str != "keyFile" && str != "sendKeyFile" &&
                    str != "sendX509" && str != "x509") { 
                        return Status(ErrorCodes::BadValue, mongoutils::str::stream() <<
                                    "Invalid value for clusterAuthMode via setParameter command: "
                                    << str);
                }

                int oldMode = serverGlobalParams.clusterAuthMode.load();
                int sslMode = sslGlobalParams.sslMode.load();
                if (str == "sendX509" && 
                    oldMode == ServerGlobalParams::ClusterAuthMode_sendKeyFile) {
                    if (sslMode == SSLGlobalParams::SSLMode_disabled ||
                        sslMode == SSLGlobalParams::SSLMode_allowSSL) {
                        return Status(ErrorCodes::BadValue, mongoutils::str::stream() <<
                                    "Illegal state transition for clusterAuthMode, " <<
                                    "need to enable SSL for outgoing connections");
                    }
                    serverGlobalParams.clusterAuthMode.store
                        (ServerGlobalParams::ClusterAuthMode_sendX509);
#ifdef MONGO_SSL
                    setInternalUserAuthParams(BSON(saslCommandMechanismFieldName << 
                                              "MONGODB-X509" <<
                                              saslCommandUserDBFieldName << "$external" <<
                                              saslCommandUserFieldName << 
                                              getSSLManager()->getClientSubjectName()));
#endif 
                }
                else if (str == "x509" && 
                    oldMode == ServerGlobalParams::ClusterAuthMode_sendX509) {
                    serverGlobalParams.clusterAuthMode.store
                        (ServerGlobalParams::ClusterAuthMode_x509);
                }
                else {
                    return Status(ErrorCodes::BadValue, mongoutils::str::stream() <<
                                  "Illegal state transition for clusterAuthMode, change from "
                                  << clusterAuthModeStr() << " to " << str);
                }
                return Status::OK();
            }
        } clusterAuthModeSetting;

        ExportedServerParameter<bool> QuietSetting( ServerParameterSet::getGlobal(),
                                                    "quiet",
                                                    &serverGlobalParams.quiet,
                                                    true,
                                                    true );

        ExportedServerParameter<int> MaxConsecutiveFailedChecksSetting(
                                                    ServerParameterSet::getGlobal(),
                                                    "replMonitorMaxFailedChecks",
                                                    &ReplicaSetMonitor::maxConsecutiveFailedChecks,
                                                    false, // allowedToChangeAtStartup
                                                    true); // allowedToChangeAtRuntime

        ExportedServerParameter<bool> TraceExceptionsSetting(ServerParameterSet::getGlobal(),
                                                             "traceExceptions",
                                                             &DBException::traceExceptions,
                                                             false, // allowedToChangeAtStartup
                                                             true); // allowedToChangeAtRuntime
    }

}

