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

#include "mongo/platform/basic.h"

#include <set>

#include "mongo/bson/json.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/config.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/commands.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/parse_log_component_settings.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"

using std::string;
using std::stringstream;

namespace mongo {

namespace {
void appendParameterNames(stringstream& help) {
    help << "supported:\n";
    const ServerParameter::Map& m = ServerParameterSet::getGlobal()->getMap();
    for (ServerParameter::Map::const_iterator i = m.begin(); i != m.end(); ++i) {
        help << "  " << i->first << "\n";
    }
}
}

class CmdGet : public Command {
public:
    CmdGet() : Command("getParameter") {}
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::getParameter);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    virtual void help(stringstream& help) const {
        help << "get administrative option(s)\nexample:\n";
        help << "{ getParameter:1, notablescan:1 }\n";
        appendParameterNames(help);
        help << "{ getParameter:'*' } to get everything\n";
    }
    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        bool all = *cmdObj.firstElement().valuestrsafe() == '*';

        int before = result.len();

        const ServerParameter::Map& m = ServerParameterSet::getGlobal()->getMap();
        for (ServerParameter::Map::const_iterator i = m.begin(); i != m.end(); ++i) {
            if (all || cmdObj.hasElement(i->first.c_str())) {
                i->second->append(txn, result, i->second->name());
            }
        }

        if (before == result.len()) {
            errmsg = "no option found to get";
            return false;
        }
        return true;
    }
} cmdGet;

class CmdSet : public Command {
public:
    CmdSet() : Command("setParameter") {}
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::setParameter);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    virtual void help(stringstream& help) const {
        help << "set administrative option(s)\n";
        help << "{ setParameter:1, <param>:<value> }\n";
        appendParameterNames(help);
    }
    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        int numSet = 0;
        bool found = false;

        const ServerParameter::Map& parameterMap = ServerParameterSet::getGlobal()->getMap();

        // First check that we aren't setting the same parameter twice and that we actually are
        // setting parameters that we have registered and can change at runtime
        BSONObjIterator parameterCheckIterator(cmdObj);

        // We already know that "setParameter" will be the first element in this object, so skip
        // past that
        parameterCheckIterator.next();

        // Set of all the parameters the user is attempting to change
        std::map<std::string, BSONElement> parametersToSet;

        // Iterate all parameters the user passed in to do the initial validation checks,
        // including verifying that we are not setting the same parameter twice.
        while (parameterCheckIterator.more()) {
            BSONElement parameter = parameterCheckIterator.next();
            std::string parameterName = parameter.fieldName();

            ServerParameter::Map::const_iterator foundParameter = parameterMap.find(parameterName);

            // Check to see if this is actually a valid parameter
            if (foundParameter == parameterMap.end()) {
                errmsg = str::stream() << "attempted to set unrecognized parameter ["
                                       << parameterName << "], use help:true to see options ";
                return false;
            }

            // Make sure we are allowed to change this parameter
            if (!foundParameter->second->allowedToChangeAtRuntime()) {
                errmsg = str::stream() << "not allowed to change [" << parameterName
                                       << "] at runtime";
                return false;
            }

            // Make sure we are only setting this parameter once
            if (parametersToSet.count(parameterName)) {
                errmsg = str::stream()
                    << "attempted to set parameter [" << parameterName
                    << "] twice in the same setParameter command, "
                    << "once to value: [" << parametersToSet[parameterName].toString(false)
                    << "], and once to value: [" << parameter.toString(false) << "]";
                return false;
            }

            parametersToSet[parameterName] = parameter;
        }

        // Iterate the parameters that we have confirmed we are setting and actually set them.
        // Not that if setting any one parameter fails, the command will fail, but the user
        // won't see what has been set and what hasn't.  See SERVER-8552.
        for (std::map<std::string, BSONElement>::iterator it = parametersToSet.begin();
             it != parametersToSet.end();
             ++it) {
            BSONElement parameter = it->second;
            std::string parameterName = it->first;

            ServerParameter::Map::const_iterator foundParameter = parameterMap.find(parameterName);

            if (foundParameter == parameterMap.end()) {
                errmsg = str::stream() << "Parameter: " << parameterName << " that was "
                                       << "avaliable during our first lookup in the registered "
                                       << "parameters map is no longer available.";
                return false;
            }

            if (numSet == 0) {
                foundParameter->second->append(txn, result, "was");
            }

            Status status = foundParameter->second->set(parameter);
            if (status.isOK()) {
                numSet++;
                continue;
            }

            errmsg = status.reason();
            result.append("code", status.code());
            return false;
        }

        if (numSet == 0 && !found) {
            errmsg = "no option found to set, use help:true to see options ";
            return false;
        }

        return true;
    }
} cmdSet;

namespace {
using logger::globalLogDomain;
using logger::LogComponent;
using logger::LogComponentSetting;
using logger::LogSeverity;
using logger::parseLogComponentSettings;

class LogLevelSetting : public ServerParameter {
public:
    LogLevelSetting() : ServerParameter(ServerParameterSet::getGlobal(), "logLevel") {}

    virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name) {
        b << name << globalLogDomain()->getMinimumLogSeverity().toInt();
    }

    virtual Status set(const BSONElement& newValueElement) {
        int newValue;
        if (!newValueElement.coerce(&newValue) || newValue < 0)
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "Invalid value for logLevel: "
                                                    << newValueElement);
        LogSeverity newSeverity =
            (newValue > 0) ? LogSeverity::Debug(newValue) : LogSeverity::Log();
        globalLogDomain()->setMinimumLoggedSeverity(newSeverity);
        return Status::OK();
    }

    virtual Status setFromString(const std::string& str) {
        int newValue;
        Status status = parseNumberFromString(str, &newValue);
        if (!status.isOK())
            return status;
        if (newValue < 0)
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "Invalid value for logLevel: " << newValue);
        LogSeverity newSeverity =
            (newValue > 0) ? LogSeverity::Debug(newValue) : LogSeverity::Log();
        globalLogDomain()->setMinimumLoggedSeverity(newSeverity);
        return Status::OK();
    }
} logLevelSetting;

/**
 * Log component verbosity.
 * Log levels of log component hierarchy.
 * Negative value for a log component means the default log level will be used.
 */
class LogComponentVerbositySetting : public ServerParameter {
    MONGO_DISALLOW_COPYING(LogComponentVerbositySetting);

public:
    LogComponentVerbositySetting()
        : ServerParameter(ServerParameterSet::getGlobal(), "logComponentVerbosity") {}

    virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name) {
        BSONObj currentSettings;
        _get(&currentSettings);
        b << name << currentSettings;
    }

    virtual Status set(const BSONElement& newValueElement) {
        if (!newValueElement.isABSONObj()) {
            return Status(ErrorCodes::TypeMismatch,
                          mongoutils::str::stream()
                              << "log component verbosity is not a BSON object: "
                              << newValueElement);
        }
        return _set(newValueElement.Obj());
    }

    virtual Status setFromString(const std::string& str) {
        try {
            return _set(mongo::fromjson(str));
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }

private:
    /**
     * Returns current settings as a BSON document.
     * The "default" log component is an implementation detail. Don't expose this to users.
     */
    void _get(BSONObj* output) const {
        static const string defaultLogComponentName =
            LogComponent(LogComponent::kDefault).getShortName();

        mutablebson::Document doc;

        for (int i = 0; i < int(LogComponent::kNumLogComponents); ++i) {
            LogComponent component = static_cast<LogComponent::Value>(i);

            int severity = -1;
            if (globalLogDomain()->hasMinimumLogSeverity(component)) {
                severity = globalLogDomain()->getMinimumLogSeverity(component).toInt();
            }

            // Save LogComponent::kDefault LogSeverity at root
            if (component == LogComponent::kDefault) {
                doc.root().appendInt("verbosity", severity);
                continue;
            }

            mutablebson::Element element = doc.makeElementObject(component.getShortName());
            element.appendInt("verbosity", severity);

            mutablebson::Element parentElement = _getParentElement(doc, component);
            parentElement.pushBack(element);
        }

        BSONObj result = doc.getObject();
        output->swap(result);
        invariant(!output->hasField(defaultLogComponentName));
    }

    /**
     * Updates component hierarchy log levels.
     *
     * BSON Format:
     * {
     *     verbosity: 4,  <-- maps to 'default' log component.
     *     componentA: {
     *         verbosity: 2,  <-- sets componentA's log level to 2.
     *         componentB: {
     *             verbosity: 1, <-- sets componentA.componentB's log level to 1.
     *         }
     *         componentC: {
     *             verbosity: -1, <-- clears componentA.componentC's log level so that
     *                                its final loglevel will be inherited from componentA.
     *         }
     *     },
     *     componentD : 3  <-- sets componentD's log level to 3 (alternative to
     *                         subdocument with 'verbosity' field).
     * }
     *
     * For the default component, the log level is read from the top-level
     * "verbosity" field.
     * For non-default components, we look up the element using the component's
     * dotted name. If the "<dotted component name>" field is a number, the log
     * level will be read from the field's value.
     * Otherwise, we assume that the "<dotted component name>" field is an
     * object with a "verbosity" field that holds the log level for the component.
     * The more verbose format with the "verbosity" field is intended to support
     * setting of log levels of both parent and child log components in the same
     * BSON document.
     *
     * Ignore elements in BSON object that do not map to a log component's dotted
     * name.
     */
    Status _set(const BSONObj& bsonSettings) const {
        StatusWith<std::vector<LogComponentSetting>> parseStatus =
            parseLogComponentSettings(bsonSettings);

        if (!parseStatus.isOK()) {
            return parseStatus.getStatus();
        }

        std::vector<LogComponentSetting> settings = parseStatus.getValue();
        std::vector<LogComponentSetting>::iterator it = settings.begin();
        for (; it < settings.end(); ++it) {
            LogComponentSetting newSetting = *it;

            // Negative value means to clear log level of component.
            if (newSetting.level < 0) {
                globalLogDomain()->clearMinimumLoggedSeverity(newSetting.component);
                continue;
            }
            // Convert non-negative value to Log()/Debug(N).
            LogSeverity newSeverity =
                (newSetting.level > 0) ? LogSeverity::Debug(newSetting.level) : LogSeverity::Log();
            globalLogDomain()->setMinimumLoggedSeverity(newSetting.component, newSeverity);
        }

        return Status::OK();
    }

    /**
     * Search document for element corresponding to log component's parent.
     */
    static mutablebson::Element _getParentElement(mutablebson::Document& doc,
                                                  LogComponent component) {
        // Hide LogComponent::kDefault
        if (component == LogComponent::kDefault) {
            return doc.end();
        }
        LogComponent parentComponent = component.parent();

        // Attach LogComponent::kDefault children to root
        if (parentComponent == LogComponent::kDefault) {
            return doc.root();
        }
        mutablebson::Element grandParentElement = _getParentElement(doc, parentComponent);
        return grandParentElement.findFirstChildNamed(parentComponent.getShortName());
    }
} logComponentVerbositySetting;

}  // namespace

namespace {
class SSLModeSetting : public ServerParameter {
public:
    SSLModeSetting()
        : ServerParameter(ServerParameterSet::getGlobal(),
                          "sslMode",
                          false,  // allowedToChangeAtStartup
                          true    // allowedToChangeAtRuntime
                          ) {}

    std::string sslModeStr() {
        switch (sslGlobalParams.sslMode.load()) {
            case SSLParams::SSLMode_disabled:
                return "disabled";
            case SSLParams::SSLMode_allowSSL:
                return "allowSSL";
            case SSLParams::SSLMode_preferSSL:
                return "preferSSL";
            case SSLParams::SSLMode_requireSSL:
                return "requireSSL";
            default:
                return "undefined";
        }
    }

    virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name) {
        b << name << sslModeStr();
    }

    virtual Status set(const BSONElement& newValueElement) {
        try {
            return setFromString(newValueElement.String());
        } catch (const MsgAssertionException& msg) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream()
                              << "Invalid value for sslMode via setParameter command: "
                              << newValueElement
                              << ", exception: "
                              << msg.what());
        }
    }

    virtual Status setFromString(const std::string& str) {
#ifndef MONGO_CONFIG_SSL
        return Status(ErrorCodes::IllegalOperation,
                      mongoutils::str::stream()
                          << "Unable to set sslMode, SSL support is not compiled into server");
#endif
        if (str != "disabled" && str != "allowSSL" && str != "preferSSL" && str != "requireSSL") {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream()
                              << "Invalid value for sslMode via setParameter command: "
                              << str);
        }

        int oldMode = sslGlobalParams.sslMode.load();
        if (str == "preferSSL" && oldMode == SSLParams::SSLMode_allowSSL) {
            sslGlobalParams.sslMode.store(SSLParams::SSLMode_preferSSL);
        } else if (str == "requireSSL" && oldMode == SSLParams::SSLMode_preferSSL) {
            sslGlobalParams.sslMode.store(SSLParams::SSLMode_requireSSL);
        } else {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream()
                              << "Illegal state transition for sslMode, attempt to change from "
                              << sslModeStr()
                              << " to "
                              << str);
        }
        return Status::OK();
    }
} sslModeSetting;

class ClusterAuthModeSetting : public ServerParameter {
public:
    ClusterAuthModeSetting()
        : ServerParameter(ServerParameterSet::getGlobal(),
                          "clusterAuthMode",
                          false,  // allowedToChangeAtStartup
                          true    // allowedToChangeAtRuntime
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

    virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name) {
        b << name << clusterAuthModeStr();
    }

    virtual Status set(const BSONElement& newValueElement) {
        try {
            return setFromString(newValueElement.String());
        } catch (const MsgAssertionException& msg) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream()
                              << "Invalid value for clusterAuthMode via setParameter command: "
                              << newValueElement
                              << ", exception: "
                              << msg.what());
        }
    }

    virtual Status setFromString(const std::string& str) {
#ifndef MONGO_CONFIG_SSL
        return Status(ErrorCodes::IllegalOperation,
                      mongoutils::str::stream() << "Unable to set clusterAuthMode, "
                                                << "SSL support is not compiled into server");
#endif
        if (str != "keyFile" && str != "sendKeyFile" && str != "sendX509" && str != "x509") {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream()
                              << "Invalid value for clusterAuthMode via setParameter command: "
                              << str);
        }

        int oldMode = serverGlobalParams.clusterAuthMode.load();
        int sslMode = sslGlobalParams.sslMode.load();
        if (str == "sendX509" && oldMode == ServerGlobalParams::ClusterAuthMode_sendKeyFile) {
            if (sslMode == SSLParams::SSLMode_disabled || sslMode == SSLParams::SSLMode_allowSSL) {
                return Status(ErrorCodes::BadValue,
                              mongoutils::str::stream()
                                  << "Illegal state transition for clusterAuthMode, "
                                  << "need to enable SSL for outgoing connections");
            }
            serverGlobalParams.clusterAuthMode.store(ServerGlobalParams::ClusterAuthMode_sendX509);
#ifdef MONGO_CONFIG_SSL
            setInternalUserAuthParams(
                BSON(saslCommandMechanismFieldName
                     << "MONGODB-X509"
                     << saslCommandUserDBFieldName
                     << "$external"
                     << saslCommandUserFieldName
                     << getSSLManager()->getSSLConfiguration().clientSubjectName));
#endif
        } else if (str == "x509" && oldMode == ServerGlobalParams::ClusterAuthMode_sendX509) {
            serverGlobalParams.clusterAuthMode.store(ServerGlobalParams::ClusterAuthMode_x509);
        } else {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream()
                              << "Illegal state transition for clusterAuthMode, change from "
                              << clusterAuthModeStr()
                              << " to "
                              << str);
        }
        return Status::OK();
    }
} clusterAuthModeSetting;

ExportedServerParameter<bool, ServerParameterType::kStartupAndRuntime> QuietSetting(
    ServerParameterSet::getGlobal(), "quiet", &serverGlobalParams.quiet);

ExportedServerParameter<int, ServerParameterType::kRuntimeOnly> MaxConsecutiveFailedChecksSetting(
    ServerParameterSet::getGlobal(),
    "replMonitorMaxFailedChecks",
    &ReplicaSetMonitor::maxConsecutiveFailedChecks);

ExportedServerParameter<bool, ServerParameterType::kRuntimeOnly> TraceExceptionsSetting(
    ServerParameterSet::getGlobal(), "traceExceptions", &DBException::traceExceptions);

class AutomationServiceDescriptor final : public ServerParameter {
public:
    static constexpr auto kName = "automationServiceDescriptor"_sd;
    static constexpr auto kMaxSize = 64U;

    AutomationServiceDescriptor()
        : ServerParameter(ServerParameterSet::getGlobal(), kName.toString(), true, true) {}

    virtual void append(OperationContext* txn,
                        BSONObjBuilder& builder,
                        const std::string& name) override {
        const stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (!_value.empty())
            builder << name << _value;
    }

    virtual Status set(const BSONElement& newValueElement) override {
        if (newValueElement.type() != mongo::String)
            return {ErrorCodes::TypeMismatch,
                    mongoutils::str::stream() << "Value for parameter " << kName
                                              << " must be of type 'string'"};
        return setFromString(newValueElement.String());
    }

    virtual Status setFromString(const std::string& str) override {
        if (str.size() > kMaxSize)
            return {ErrorCodes::Overflow,
                    mongoutils::str::stream() << "Value for parameter " << kName
                                              << " must be no more than "
                                              << kMaxSize
                                              << " bytes"};

        {
            const stdx::lock_guard<stdx::mutex> lock(_mutex);
            _value = str;
        }

        return Status::OK();
    }

private:
    stdx::mutex _mutex;
    std::string _value;
} automationServiceDescriptor;

constexpr decltype(AutomationServiceDescriptor::kName) AutomationServiceDescriptor::kName;
}
}
