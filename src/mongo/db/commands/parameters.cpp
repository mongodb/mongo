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

#include <set>

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/parameters_gen.h"
#include "mongo/db/commands/parse_log_component_settings.h"
#include "mongo/db/server_parameter_gen.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

using logv2::LogComponent;
using logv2::LogSeverity;

void appendParameterNames(std::string* help) {
    *help += "supported:\n";
    for (const auto& kv : ServerParameterSet::getNodeParameterSet()->getMap()) {
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
    static const std::string defaultLogComponentName =
        LogComponent(LogComponent::kDefault).getShortName();

    mutablebson::Document doc;

    for (int i = 0; i < int(LogComponent::kNumLogComponents); ++i) {
        LogComponent component = static_cast<LogComponent::Value>(i);

        int severity = -1;
        if (logv2::LogManager::global().getGlobalSettings().hasMinimumLogSeverity(component)) {
            severity = logv2::LogManager::global()
                           .getGlobalSettings()
                           .getMinimumLogSeverity(component)
                           .toInt();
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
            logv2::LogManager::global().getGlobalSettings().clearMinimumLoggedSeverity(
                newSetting.component);
            continue;
        }
        // Convert non-negative value to Log()/Debug(N).
        LogSeverity newSeverity =
            (newSetting.level > 0) ? LogSeverity::Debug(newSetting.level) : LogSeverity::Log();
        logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
            newSetting.component, newSeverity);
    }

    return Status::OK();
}

GetParameterOptions parseGetParameterOptions(BSONElement elem) {
    if (elem.type() == BSONType::Object) {
        return GetParameterOptions::parse(IDLParserContext{"getParameter"}, elem.Obj());
    }
    if ((elem.type() == BSONType::String) && (elem.valueStringDataSafe() == "*"_sd)) {
        GetParameterOptions ret;
        ret.setAllParameters(true);
        return ret;
    }
    return GetParameterOptions{};
}

// for automationServiceDescription
Mutex autoServiceDescriptorMutex;
std::string autoServiceDescriptorValue;
}  // namespace

class CmdGet : public BasicCommand {
public:
    CmdGet() : BasicCommand("getParameter") {}
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
            "{ getParameter:1, notablescan:1 }\n"
            "pass a document as the value for getParameter to request options\nexample:\n"
            "{ getParameter:{showDetails: true}, notablescan:1}\n";
        appendParameterNames(&h);
        h += "{ getParameter:'*' } or { getParameter:{allParameters: true} } to get everything\n";
        return h;
    }
    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const auto options = parseGetParameterOptions(cmdObj.firstElement());
        const bool all = options.getAllParameters();

        int before = result.len();

        const ServerParameter::Map& m = ServerParameterSet::getNodeParameterSet()->getMap();
        for (ServerParameter::Map::const_iterator i = m.begin(); i != m.end(); ++i) {
            if (all || cmdObj.hasElement(i->first.c_str())) {
                if (options.getShowDetails()) {
                    BSONObjBuilder detailBob(result.subobjStart(i->second->name()));
                    i->second->append(opCtx, detailBob, "value");
                    detailBob.appendBool("settableAtRuntime",
                                         i->second->allowedToChangeAtRuntime());
                    detailBob.appendBool("settableAtStartup",
                                         i->second->allowedToChangeAtStartup());
                    detailBob.doneFast();
                } else {
                    i->second->append(opCtx, result, i->second->name());
                }
            }
        }
        uassert(ErrorCodes::InvalidOptions, "no option found to get", before != result.len());
        return true;
    }
} cmdGet;

class CmdSet : public BasicCommand {
public:
    CmdSet() : BasicCommand("setParameter") {}
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
    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        int numSet = 0;
        bool found = false;

        const ServerParameter::Map& parameterMap =
            ServerParameterSet::getNodeParameterSet()->getMap();

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
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "attempted to set unrecognized parameter [" << parameterName
                                  << "], use help:true to see options ",
                    foundParameter != parameterMap.end());

            // Make sure we are allowed to change this parameter
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "not allowed to change [" << parameterName << "] at runtime",
                    foundParameter->second->allowedToChangeAtRuntime());

            // Make sure we are only setting this parameter once
            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "attempted to set parameter [" << parameterName
                                  << "] twice in the same setParameter command, "
                                  << "once to value: ["
                                  << parametersToSet[parameterName].toString(false)
                                  << "], and once to value: [" << parameter.toString(false) << "]",
                    parametersToSet.count(parameterName) == 0);

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

            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << "Parameter: " << parameterName << " that was "
                                  << "avaliable during our first lookup in the registered "
                                  << "parameters map is no longer available.",
                    foundParameter != parameterMap.end());

            uassert(
                ErrorCodes::IllegalOperation,
                str::stream()
                    << "Cannot set parameter requireApiVersion=true on a shard or config server",
                parameterName != "requireApiVersion" || !parameter.trueValue() ||
                    (serverGlobalParams.clusterRole != ClusterRole::ConfigServer &&
                     serverGlobalParams.clusterRole != ClusterRole::ShardServer));

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
                LOGV2(20496,
                      "Error setting parameter {parameterName} to {newValue} errMsg: {error}",
                      "Error setting parameter to new value",
                      "parameterName"_attr = parameterName,
                      "newValue"_attr = redact(parameter.toString(false)),
                      "error"_attr = redact(ex));
                throw;
            }

            if (parameterName == "logComponentVerbosity") {
                const BSONObj obj = parameter.Obj();
                auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
                if (storageEngine != nullptr && obj.hasField("storage") &&
                    obj.getObjectField("storage").hasField("wt")) {
                    uassertStatusOK(storageEngine->getEngine()->reconfigureLogging());
                }
            }

            if (oldValue) {
                LOGV2(23435,
                      "Successfully set parameter {parameterName} to {newValue} (was "
                      "{oldValue})",
                      "Successfully set parameter to new value",
                      "parameterName"_attr = parameterName,
                      "newValue"_attr = redact(parameter.toString(false)),
                      "oldValue"_attr = redact(oldValue.toString(false)));
            } else {
                LOGV2(23436,
                      "Successfully set parameter {parameterName} to {newValue}",
                      "Successfully set parameter to new value",
                      "parameterName"_attr = parameterName,
                      "newValue"_attr = redact(parameter.toString(false)));
            }

            numSet++;
        }

        uassert(ErrorCodes::InvalidOptions,
                "no option found to set, use help:true to see options ",
                numSet != 0 || found);

        return true;
    }
} cmdSet;

void LogLevelServerParameter::append(OperationContext*,
                                     BSONObjBuilder& builder,
                                     const std::string& name) {
    builder.append(name,
                   logv2::LogManager::global()
                       .getGlobalSettings()
                       .getMinimumLogSeverity(mongo::logv2::LogComponent::kDefault)
                       .toInt());
}

Status LogLevelServerParameter::set(const BSONElement& newValueElement) {
    int newValue;
    if (!newValueElement.coerce(&newValue) || newValue < 0)
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid value for logLevel: " << newValueElement);
    LogSeverity newSeverity = (newValue > 0) ? LogSeverity::Debug(newValue) : LogSeverity::Log();

    logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
        mongo::logv2::LogComponent::kDefault, newSeverity);
    return Status::OK();
}

Status LogLevelServerParameter::setFromString(const std::string& strLevel) {
    int newValue;
    Status status = NumberParser{}(strLevel, &newValue);
    if (!status.isOK())
        return status;
    if (newValue < 0)
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid value for logLevel: " << newValue);
    LogSeverity newSeverity = (newValue > 0) ? LogSeverity::Debug(newValue) : LogSeverity::Log();

    logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
        mongo::logv2::LogComponent::kDefault, newSeverity);
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
    const stdx::lock_guard<Latch> lock(autoServiceDescriptorMutex);
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
        const stdx::lock_guard<Latch> lock(autoServiceDescriptorMutex);
        autoServiceDescriptorValue = str;
    }

    return Status::OK();
}

}  // namespace mongo
