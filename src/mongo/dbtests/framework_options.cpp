// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/dbtests/framework_options.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/db/admission/flow_control_parameters_gen.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/unittest/log_test.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/options_parser/value.h"
#include "mongo/util/quick_exit.h"

#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/exception.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::dbtests {
namespace {

namespace fs = boost::filesystem;
namespace moe = optionenvironment;  // NOLINT(misc-unused-alias-decls)

std::vector<std::string>& suitesSingleton() {
    static auto& obj = *new std::vector<std::string>;
    return obj;
}

std::string getTestFrameworkHelp(std::string_view name, const moe::OptionSection& options) {
    return fmt::format(
        "usage: {} [options] [suite]...\n"
        "{}"
        "suite: run the specified test suite(s) only\n",
        name,
        options.helpString());
}

/** Remove the contents of the test directory if it exists. */
Status ensureCleanDir(fs::path p, const std::string& exe) {
    try {
        if (exists(p)) {
            if (!is_directory(p)) {
                return Status(ErrorCodes::BadValue,
                              fmt::format("ERROR: path {:?} is not a directory {}",
                                          p.string(),
                                          getTestFrameworkHelp(exe, moe::startupOptions)));
            }
            std::for_each(fs::directory_iterator{p}, fs::directory_iterator{}, [](auto&& f) {
                remove_all(f);
            });
        } else {
            create_directory(p);
        }
        return Status::OK();
    } catch (const fs::filesystem_error& e) {
        return Status(ErrorCodes::BadValue,
                      fmt::format("boost::filesystem threw exception: {}", e.what()));
    }
}

MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(FrameworkOptions)(InitializerContext*) {
    auto& options = moe::startupOptions;

    options.addOptionChaining("help", "help,h", moe::Switch, "Show this usage information", {}, {})
        .setSources(moe::SourceCommandLine);

    options
        .addOptionChaining("dbpath",
                           "dbpath",
                           moe::String,
                           "db data path for this test run. NOTE: the contents of this directory "
                           "will be overwritten if it already exists",
                           {},
                           {})
        .setSources(moe::SourceCommandLine)
        .setDefault(moe::Value("/tmp/unittest"));

    options
        .addOptionChaining(
            "storage.engine", "storageEngine", moe::String, "What storage engine to use", {}, {})
        .setSources(moe::SourceCommandLine)
        .setDefault(moe::Value("wiredTiger"));

    options
        .addOptionChaining("enableFlowControl",
                           "flowControl",
                           moe::Bool,
                           "Whether Flow Control is enabled",
                           {},
                           {})
        .setSources(moe::SourceCommandLine)
        .setDefault(moe::Value(true));

    options
        .addOptionChaining(
            "setParameter", "setParameter", moe::StringMap, "Set a configurable parameter", {}, {})
        .setSources(moe::SourceCommandLine)
        .composing();

    options.addOptionChaining("suites", "suites", moe::StringVector, "Test suites to run", {}, {})
        .setSources(moe::SourceCommandLine)
        .hidden()
        .positional(1, -1);
}

MONGO_STARTUP_OPTIONS_VALIDATE(FrameworkOptions)(InitializerContext* context) {
    const std::vector<std::string>& args = context->args();
    moe::Environment& params = moe::startupOptionsParsed;
    if (params.count("help")) {
        std::cout << getTestFrameworkHelp(args[0], moe::startupOptions) << std::endl;
        quickExit(ExitCode::clean);
    }
    uassertStatusOK(params.validate());
}

MONGO_STARTUP_OPTIONS_STORE(FrameworkOptions)(InitializerContext* context) try {
    const std::vector<std::string>& args = context->args();
    const moe::Environment& params = moe::startupOptionsParsed;
    if (kDebugBuild)
        LOGV2(22491, "DEBUG build");

    if (params.count("debug") || params.count("verbose")) {
        unittest::setMinimumLoggedSeverity(logv2::LogSeverity::Debug(1));
    }

    if (params.count("dbpath")) {
        auto p = params["dbpath"].as<std::string>();
        uassertStatusOK(ensureCleanDir(p, args.front()));
        storageGlobalParams.dbpath = p;
    }

    storageGlobalParams.engine = params["storage.engine"].as<std::string>();

    gFlowControlEnabled.store(params["enableFlowControl"].as<bool>());
    if (gFlowControlEnabled.load())
        LOGV2(22492, "Flow Control enabled");

    if (params.count("setParameter")) {
        auto parameters = params["setParameter"].as<std::map<std::string, std::string>>();
        auto* paramSet = ServerParameterSet::getNodeParameterSet();
        for (auto&& [name, value] : parameters) {
            auto parameter = paramSet->getIfExists(name);
            uassert(
                {ErrorCodes::BadValue, fmt::format("Illegal --setParameter parameter: {:?}", name)},
                parameter);
            uassert({ErrorCodes::BadValue,
                     fmt::format("Cannot use --setParameter to set {:?} at startup", name)},
                    parameter->allowedToChangeAtStartup());
            if (auto err = parameter->setFromString(value, boost::none); !err.isOK()) {
                uasserted({ErrorCodes::BadValue,
                           fmt::format("Bad value for parameter {:?}: {}", name, err.reason())});
            }
            LOGV2(
                4539300, "Setting server parameter", "parameter"_attr = name, "value"_attr = value);
        }
    }

    if (params.count("suites"))
        suitesSingleton() = params["suites"].as<std::vector<std::string>>();
} catch (const DBException& ex) {
    std::cerr << ex.toString() << std::endl;
    std::cerr << "try '" << context->args()[0] << " --help' for more information" << std::endl;
    quickExit(ExitCode::badOptions);
}

}  // namespace

std::vector<std::string> getFrameworkSuites() {
    return suitesSingleton();
}

}  // namespace mongo::dbtests
