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

#include "mongo/db/server_options_helpers.h"

#ifdef _WIN32
#include <direct.h>
#else
#define SYSLOG_NAMES
#include <syslog.h>
#endif
#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <fmt/format.h>
#include <ios>
#include <iostream>

#include "mongo/base/status.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/builder.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_component_settings.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

/*
 * SERVER-11160 syslog.h does not define facilitynames under solaris.
 * syslog.h exports preprocessor macro INTERNAL_NOPRI if
 * facilitynames is provided. This will be used to determine
 * if facilitynames should be defined here.
 * These could also go into a syslog.h compatibility header.
 * We are using INTERNAL_NOPRI as the indicator macro for facilitynames
 * because it's defined alongside facilitynames in the syslog.h headers
 * that support SYSLOG_NAMES.
 */

namespace {

#if defined(SYSLOG_NAMES)
#if !defined(INTERNAL_NOPRI)

typedef struct _code {
    const char* c_name;
    int c_val;
} CODE;

CODE facilitynames[] = {{"auth", LOG_AUTH},     {"cron", LOG_CRON},     {"daemon", LOG_DAEMON},
                        {"kern", LOG_KERN},     {"lpr", LOG_LPR},       {"mail", LOG_MAIL},
                        {"news", LOG_NEWS},     {"security", LOG_AUTH}, /* DEPRECATED */
                        {"syslog", LOG_SYSLOG}, {"user", LOG_USER},     {"uucp", LOG_UUCP},
                        {"local0", LOG_LOCAL0}, {"local1", LOG_LOCAL1}, {"local2", LOG_LOCAL2},
                        {"local3", LOG_LOCAL3}, {"local4", LOG_LOCAL4}, {"local5", LOG_LOCAL5},
                        {"local6", LOG_LOCAL6}, {"local7", LOG_LOCAL7}, {nullptr, -1}};

#endif  // !defined(INTERNAL_NOPRI)
#endif  // defined(SYSLOG_NAMES)

Status setArgvArray(const std::vector<std::string>& argv) {
    BSONArrayBuilder b;
    std::vector<std::string> censoredArgv = argv;
    cmdline_utils::censorArgsVector(&censoredArgv);
    for (size_t i = 0; i < censoredArgv.size(); i++) {
        b << censoredArgv[i];
    }
    serverGlobalParams.argvArray = b.arr();
    return Status::OK();
}

Status setParsedOpts(const moe::Environment& params) {
    serverGlobalParams.parsedOpts = params.toBSON();
    cmdline_utils::censorBSONObj(&serverGlobalParams.parsedOpts);
    return Status::OK();
}
}  // namespace

Status validateBaseOptions(const moe::Environment& params) {
    if (params.count("verbose")) {
        std::string verbosity = params["verbose"].as<std::string>();

        // Skip this for backwards compatibility.  See SERVER-11471.
        if (verbosity != "true") {
            for (std::string::iterator iterator = verbosity.begin(); iterator != verbosity.end();
                 iterator++) {
                if (*iterator != 'v') {
                    return Status(ErrorCodes::BadValue,
                                  "The \"verbose\" option string cannot contain any characters "
                                  "other than \"v\"");
                }
            }
        }
    }

    if (params.count("setParameter")) {
        const auto parameters = params["setParameter"].as<std::map<std::string, std::string>>();

        const bool enableTestCommandsValue = ([&parameters] {
            const auto etc = parameters.find("enableTestCommands");
            if (etc == parameters.end()) {
                return false;
            }
            const auto& val = etc->second;
            return (0 == val.compare("1")) || (0 == val.compare("true"));
        })();

        if (enableTestCommandsValue) {
            // Only register failpoint server parameters if enableTestCommands=1.
            globalFailPointRegistry().registerAllFailPointsAsServerParameters();
        } else {
            // Deregister test-only parameters.
            ServerParameterSet::getNodeParameterSet()->disableTestParameters();
            ServerParameterSet::getClusterParameterSet()->disableTestParameters();
        }

        // Must come after registerAllFailPointsAsServerParameters() above.
        auto* paramSet = ServerParameterSet::getNodeParameterSet();
        for (const auto& setParam : parameters) {
            auto* param = paramSet->getIfExists(setParam.first);

            if (!param) {
                return {ErrorCodes::BadValue,
                        str::stream() << "Unknown --setParameter '" << setParam.first << "'"};
            }

            if (!param->isEnabled()) {
                return {ErrorCodes::BadValue,
                        str::stream() << "--setParameter '" << setParam.first
                                      << "' only available when used with 'enableTestCommands'"};
            }
        }
    }

    return Status::OK();
}

Status canonicalizeBaseOptions(moe::Environment* params) {
    // Handle both the "--verbose" string argument and the "-vvvv" arguments at the same time so
    // that we ensure that we set the log level to the maximum of the options provided
    int logLevel = -1;
    for (std::string s = ""; s.length() <= 14; s.append("v")) {
        if (!s.empty() && params->count(s) && (*params)[s].as<bool>() == true) {
            logLevel = s.length();
        }

        if (params->count("verbose")) {
            std::string verbosity;
            params->get("verbose", &verbosity).transitional_ignore();
            if (s == verbosity ||
                // Treat a verbosity of "true" the same as a single "v".  See SERVER-11471.
                (s == "v" && verbosity == "true")) {
                logLevel = s.length();
            }
        }

        // Remove all "v" options we have already handled
        Status ret = params->remove(s);
        if (!ret.isOK()) {
            return ret;
        }
    }

    if (logLevel != -1) {
        Status ret = params->set("systemLog.verbosity", moe::Value(logLevel));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("verbose");
        if (!ret.isOK()) {
            return ret;
        }
    }

    if (params->count("logpath")) {
        std::string logpath;
        Status ret = params->get("logpath", &logpath);
        if (!ret.isOK()) {
            return ret;
        }
        if (logpath.empty()) {
            return Status(ErrorCodes::BadValue, "logpath cannot be empty if supplied");
        }
        ret = params->set("systemLog.destination", moe::Value(std::string("file")));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->set("systemLog.path", moe::Value(logpath));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("logpath");
        if (!ret.isOK()) {
            return ret;
        }
    }

    // "systemLog.destination" comes from the config file, so override it if "syslog" is set
    // since that comes from the command line.
    if (params->count("syslog") && (*params)["syslog"].as<bool>() == true) {
        Status ret = params->set("systemLog.destination", moe::Value(std::string("syslog")));
        if (!ret.isOK()) {
            return ret;
        }
        ret = params->remove("syslog");
        if (!ret.isOK()) {
            return ret;
        }
    }

    return Status::OK();
}

Status setupBaseOptions(const std::vector<std::string>& args) {
    Status ret = setArgvArray(args);
    if (!ret.isOK()) {
        return ret;
    }

    return Status::OK();
}

namespace server_options_detail {
StatusWith<BSONObj> applySetParameterOptions(const std::map<std::string, std::string>& toApply,
                                             ServerParameterSet& parameterSet) {
    using namespace fmt::literals;
    BSONObjBuilder summaryBuilder;
    for (const auto& [name, value] : toApply) {
        auto sp = parameterSet.getIfExists(name);
        if (!sp)
            return Status(ErrorCodes::BadValue,
                          "Illegal --setParameter parameter: \"{}\""_format(name));
        if (!sp->allowedToChangeAtStartup())
            return Status(ErrorCodes::BadValue,
                          "Cannot use --setParameter to set \"{}\" at startup"_format(name));
        BSONObjBuilder sub(summaryBuilder.subobjStart(name));
        sp->append(nullptr, &sub, "default", boost::none);
        Status status = sp->setFromString(value, boost::none);
        if (!status.isOK())
            return Status(ErrorCodes::BadValue,
                          "Bad value for parameter \"{}\": {}"_format(name, status.reason()));
        sp->append(nullptr, &sub, "value", boost::none);
    }
    return summaryBuilder.obj();
}
}  // namespace server_options_detail

Status storeBaseOptions(const moe::Environment& params) {
    Status ret = setParsedOpts(params);
    if (!ret.isOK()) {
        return ret;
    }

    if (params.count("systemLog.verbosity")) {
        int verbosity = params["systemLog.verbosity"].as<int>();
        if (verbosity < 0) {
            // This can only happen in YAML config
            return Status(ErrorCodes::BadValue,
                          "systemLog.verbosity YAML Config cannot be negative");
        }
        logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
            mongo::logv2::LogComponent::kDefault, logv2::LogSeverity::Debug(verbosity));
    }

    // log component hierarchy verbosity levels
    for (int i = 0; i < int(logv2::LogComponent::kNumLogComponents); ++i) {
        logv2::LogComponent component = static_cast<logv2::LogComponent::Value>(i);
        if (component == logv2::LogComponent::kDefault) {
            continue;
        }
        const std::string dottedName =
            "systemLog.component." + component.getDottedName() + ".verbosity";
        if (params.count(dottedName)) {
            int verbosity = params[dottedName].as<int>();
            // Clear existing log level if log level is negative.
            if (verbosity < 0) {
                logv2::LogManager::global().getGlobalSettings().clearMinimumLoggedSeverity(
                    component);
            } else {
                logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
                    component, logv2::LogSeverity::Debug(verbosity));
            }
        }
    }

    if (params.count("enableExperimentalStorageDetailsCmd")) {
        serverGlobalParams.experimental.storageDetailsCmdEnabled =
            params["enableExperimentalStorageDetailsCmd"].as<bool>();
    }

    if (params.count("systemLog.quiet")) {
        serverGlobalParams.quiet.store(params["systemLog.quiet"].as<bool>());
    }

    if (params.count("systemLog.traceAllExceptions")) {
        DBException::traceExceptions.store(params["systemLog.traceAllExceptions"].as<bool>());
    }

    if (params.count("systemLog.timeStampFormat")) {
        std::string formatterName = params["systemLog.timeStampFormat"].as<std::string>();
        if (formatterName == "iso8601-utc") {
            serverGlobalParams.logTimestampFormat = logv2::LogTimestampFormat::kISO8601UTC;
            setDateFormatIsLocalTimezone(false);
        } else if (formatterName == "iso8601-local") {
            serverGlobalParams.logTimestampFormat = logv2::LogTimestampFormat::kISO8601Local;
            setDateFormatIsLocalTimezone(true);
        } else {
            StringBuilder sb;
            sb << "Value of logTimestampFormat must be one of iso8601-utc "
               << "or iso8601-local; not \"" << formatterName << "\".";
            return Status(ErrorCodes::BadValue, sb.str());
        }
    }

    if (params.count("systemLog.destination")) {
        std::string systemLogDestination = params["systemLog.destination"].as<std::string>();
        if (systemLogDestination == "file") {
            if (params.count("systemLog.path")) {
                serverGlobalParams.logpath = params["systemLog.path"].as<std::string>();
            } else {
                return Status(ErrorCodes::BadValue,
                              "systemLog.path is required if systemLog.destination is to a "
                              "file");
            }
        } else if (systemLogDestination == "syslog") {
            if (params.count("systemLog.path")) {
                return Status(ErrorCodes::BadValue,
                              "Can only use systemLog.path if systemLog.destination is to a "
                              "file");
            }
            serverGlobalParams.logWithSyslog = true;
        } else {
            StringBuilder sb;
            sb << "Bad value for systemLog.destination: " << systemLogDestination
               << ".  Supported targets are: (syslog|file)";
            return Status(ErrorCodes::BadValue, sb.str());
        }
    } else {
        if (params.count("systemLog.path")) {
            return Status(ErrorCodes::BadValue,
                          "Can only use systemLog.path if systemLog.destination is to a file");
        }
    }

#ifndef _WIN32
    if (params.count("systemLog.syslogFacility")) {
        std::string facility = params["systemLog.syslogFacility"].as<std::string>();
        bool set = false;
        // match facility string to facility value
        size_t facilitynamesLength = sizeof(facilitynames) / sizeof(facilitynames[0]);
        for (unsigned long i = 0; i < facilitynamesLength && facilitynames[i].c_name != nullptr;
             i++) {
            if (!facility.compare(facilitynames[i].c_name)) {
                serverGlobalParams.syslogFacility = facilitynames[i].c_val;
                set = true;
            }
        }
        if (!set) {
            StringBuilder sb;
            sb << "ERROR: syslogFacility must be set to a string representing one of the "
               << "possible syslog facilities";
            return Status(ErrorCodes::BadValue, sb.str());
        }
    } else {
        serverGlobalParams.syslogFacility = LOG_USER;
    }
#endif  // _WIN32

    if (params.count("systemLog.logAppend") && params["systemLog.logAppend"].as<bool>() == true) {
        serverGlobalParams.logAppend = true;
    }

    if (params.count("systemLog.logRotate")) {
        std::string logRotateParam = params["systemLog.logRotate"].as<std::string>();
        if (logRotateParam == "reopen") {
            serverGlobalParams.logRenameOnRotate = false;

            if (serverGlobalParams.logAppend == false) {
                return Status(ErrorCodes::BadValue,
                              "logAppend must equal true if logRotate is set to reopen");
            }
        } else if (logRotateParam == "rename") {
            serverGlobalParams.logRenameOnRotate = true;
        } else {
            return Status(ErrorCodes::BadValue,
                          "unsupported value for logRotate " + logRotateParam);
        }
    }

    if (!serverGlobalParams.logpath.empty() && serverGlobalParams.logWithSyslog) {
        return Status(ErrorCodes::BadValue, "Cant use both a logpath and syslog ");
    }

    if (params.count("processManagement.pidFilePath")) {
        serverGlobalParams.pidFile = params["processManagement.pidFilePath"].as<std::string>();
    }

    if (params.count("processManagement.timeZoneInfo")) {
        serverGlobalParams.timeZoneInfoPath =
            params["processManagement.timeZoneInfo"].as<std::string>();
    }

    if (params.count("setParameter")) {
        auto swObj = server_options_detail::applySetParameterOptions(
            params["setParameter"].as<std::map<std::string, std::string>>(),
            *ServerParameterSet::getNodeParameterSet());
        if (!swObj.isOK())
            return swObj.getStatus();
        if (const BSONObj& obj = swObj.getValue(); !obj.isEmpty())
            LOGV2(5760901, "Applied --setParameter options", "serverParameters"_attr = obj);
    }

    if (params.count("operationProfiling.slowOpThresholdMs")) {
        serverGlobalParams.slowMS = params["operationProfiling.slowOpThresholdMs"].as<int>();
    }

    if (params.count("operationProfiling.slowOpSampleRate")) {
        serverGlobalParams.sampleRate = params["operationProfiling.slowOpSampleRate"].as<double>();
    }

    if (params.count("operationProfiling.filter")) {
        try {
            serverGlobalParams.defaultProfileFilter =
                fromjson(params["operationProfiling.filter"].as<std::string>()).getOwned();
        } catch (AssertionException& e) {
            // Add more context to the error
            uasserted(ErrorCodes::FailedToParse,
                      str::stream()
                          << "Failed to parse option operationProfiling.filter: " << e.reason());
        }
    }

    return Status::OK();
}

}  // namespace mongo
