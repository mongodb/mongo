/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <set>

#include "mongo/bson/json.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/command_generic_argument.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/parameters_gen.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/parse_log_component_settings.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

using std::string;
using std::stringstream;

namespace mongo {

namespace {
using logger::globalLogDomain;
using logger::LogComponent;
using logger::LogComponentSetting;
using logger::LogSeverity;
using logger::parseLogComponentSettings;

void appendParameterNames(std::string* help) {
    *help += "supported:\n";
    for (const auto& kv : ServerParameterSet::getGlobal()->getMap()) {
        *help += "  ";
        *help += kv.first;
        *help += '\n';
    }
}

/**
 * Search document for element corresponding to log component's parent.
 */
static mutablebson::Element getParentForLogComponent(mutablebson::Document& doc,
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
    mutablebson::Element grandParentElement = getParentForLogComponent(doc, parentComponent);
    return grandParentElement.findFirstChildNamed(parentComponent.getShortName());
}

/**
 * Returns current settings as a BSON document.
 * The "default" log component is an implementation detail. Don't expose this to users.
 */
void getLogComponentVerbosity(BSONObj* output) {
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
            doc.root().appendInt("verbosity", severity).transitional_ignore();
            continue;
        }

        mutablebson::Element element = doc.makeElementObject(component.getShortName());
        element.appendInt("verbosity", severity).transitional_ignore();

        mutablebson::Element parentElement = getParentForLogComponent(doc, component);
        parentElement.pushBack(element).transitional_ignore();
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
Status setLogComponentVerbosity(const BSONObj& bsonSettings) {
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

// for automationServiceDescription
stdx::mutex autoServiceDescriptorMutex;
std::string autoServiceDescriptorValue;
}  // namespace

class CmdGet : public ErrmsgCommandDeprecated {
public:
    CmdGet() : ErrmsgCommandDeprecated("getParameter") {}
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::getParameter);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    std::string help() const override {
        std::string h =
            "get administrative option(s)\nexample:\n"
            "{ getParameter:1, notablescan:1 }\n";
        appendParameterNames(&h);
        h += "{ getParameter:'*' } to get everything\n";
        return h;
    }
    bool errmsgRun(OperationContext* opCtx,
                   const string& dbname,
                   const BSONObj& cmdObj,
                   string& errmsg,
                   BSONObjBuilder& result) {
        bool all = *cmdObj.firstElement().valuestrsafe() == '*';

        int before = result.len();

        const ServerParameter::Map& m = ServerParameterSet::getGlobal()->getMap();
        for (ServerParameter::Map::const_iterator i = m.begin(); i != m.end(); ++i) {
            if (all || cmdObj.hasElement(i->first.c_str())) {
                i->second->append(opCtx, result, i->second->name());
            }
        }

        if (before == result.len()) {
            errmsg = "no option found to get";
            return false;
        }
        return true;
    }
} cmdGet;

class CmdSet : public ErrmsgCommandDeprecated {
public:
    CmdSet() : ErrmsgCommandDeprecated("setParameter") {}
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::setParameter);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    std::string help() const override {
        std::string h =
            "set administrative option(s)\n"
            "{ setParameter:1, <param>:<value> }\n";
        appendParameterNames(&h);
        return h;
    }
    bool errmsgRun(OperationContext* opCtx,
                   const string& dbname,
                   const BSONObj& cmdObj,
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
            if (isGenericArgument(parameterName))
                continue;

            ServerParameter::Map::const_iterator foundParameter = parameterMap.find(parameterName);

            // Check to see if this is actually a valid parameter
            if (foundParameter == parameterMap.end()) {
                errmsg = str::stream() << "attempted to set unrecognized parameter ["
                                       << parameterName << "], use help:true to see options ";
                return false;
            }

            // Make sure we are allowed to change this parameter
            if (!foundParameter->second->allowedToChangeAtRuntime()) {
                errmsg = str::stream()
                    << "not allowed to change [" << parameterName << "] at runtime";
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

            auto oldValueObj = ([&] {
                BSONObjBuilder bb;
                if (numSet == 0) {
                    foundParameter->second->append(opCtx, bb, "was");
                }
                return bb.obj();
            })();
            auto oldValue = oldValueObj.firstElement();

            if (oldValue) {
                result.append(oldValue);
            }

            try {
                uassertStatusOK(foundParameter->second->set(parameter));
            } catch (const DBException& ex) {
                log() << "error setting parameter " << parameterName << " to "
                      << redact(parameter.toString(false)) << " errMsg: " << redact(ex);
                throw;
            }

            log() << "successfully set parameter " << parameterName << " to "
                  << redact(parameter.toString(false))
                  << (oldValue ? std::string(str::stream()
                                             << " (was " << redact(oldValue.toString(false)) << ")")
                               : "");

            numSet++;
        }

        if (numSet == 0 && !found) {
            errmsg = "no option found to set, use help:true to see options ";
            return false;
        }

        return true;
    }
} cmdSet;

void LogLevelServerParameter::append(OperationContext*,
                                     BSONObjBuilder& builder,
                                     const std::string& name) {
    builder.append(name, globalLogDomain()->getMinimumLogSeverity().toInt());
}

Status LogLevelServerParameter::set(const BSONElement& newValueElement) {
    int newValue;
    if (!newValueElement.coerce(&newValue) || newValue < 0)
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid value for logLevel: " << newValueElement);
    LogSeverity newSeverity = (newValue > 0) ? LogSeverity::Debug(newValue) : LogSeverity::Log();
    globalLogDomain()->setMinimumLoggedSeverity(newSeverity);
    return Status::OK();
}

Status LogLevelServerParameter::setFromString(const std::string& strLevel) {
    int newValue;
    Status status = parseNumberFromString(strLevel, &newValue);
    if (!status.isOK())
        return status;
    if (newValue < 0)
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid value for logLevel: " << newValue);
    LogSeverity newSeverity = (newValue > 0) ? LogSeverity::Debug(newValue) : LogSeverity::Log();
    globalLogDomain()->setMinimumLoggedSeverity(newSeverity);
    return Status::OK();
}

void LogComponentVerbosityServerParameter::append(OperationContext*,
                                                  BSONObjBuilder& builder,
                                                  const std::string& name) {
    BSONObj currentSettings;
    getLogComponentVerbosity(&currentSettings);
    builder.append(name, currentSettings);
}

Status LogComponentVerbosityServerParameter::set(const BSONElement& newValueElement) {
    if (!newValueElement.isABSONObj()) {
        return Status(ErrorCodes::TypeMismatch,
                      str::stream()
                          << "log component verbosity is not a BSON object: " << newValueElement);
    }
    return setLogComponentVerbosity(newValueElement.Obj());
}

Status LogComponentVerbosityServerParameter::setFromString(const std::string& str) try {
    return setLogComponentVerbosity(fromjson(str));
} catch (const DBException& ex) {
    return ex.toStatus();
}

void AutomationServiceDescriptorServerParameter::append(OperationContext*,
                                                        BSONObjBuilder& builder,
                                                        const std::string& name) {
    const stdx::lock_guard<stdx::mutex> lock(autoServiceDescriptorMutex);
    if (!autoServiceDescriptorValue.empty()) {
        builder.append(name, autoServiceDescriptorValue);
    }
}

Status AutomationServiceDescriptorServerParameter::set(const BSONElement& newValueElement) {
    if (newValueElement.type() != String) {
        return {ErrorCodes::TypeMismatch,
                "Value for parameter automationServiceDescriptor must be of type 'string'"};
    }
    return setFromString(newValueElement.String());
}

Status AutomationServiceDescriptorServerParameter::setFromString(const std::string& str) {
    auto kMaxSize = 64U;
    if (str.size() > kMaxSize)
        return {ErrorCodes::Overflow,
                str::stream() << "Value for parameter automationServiceDescriptor"
                              << " must be no more than " << kMaxSize << " bytes"};

    {
        const stdx::lock_guard<stdx::mutex> lock(autoServiceDescriptorMutex);
        autoServiceDescriptorValue = str;
    }

    return Status::OK();
}

}  // namespace mongo
