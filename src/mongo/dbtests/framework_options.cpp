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


#include "mongo/platform/basic.h"


#include "mongo/dbtests/framework_options.h"

#include <boost/filesystem/operations.hpp>
#include <iostream>

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/query/find.h"
#include "mongo/db/storage/flow_control_parameters_gen.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/password.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {

using std::cout;
using std::endl;
using std::string;
using std::vector;

FrameworkGlobalParams frameworkGlobalParams;

std::string getTestFrameworkHelp(StringData name, const moe::OptionSection& options) {
    StringBuilder sb;
    sb << "usage: " << name << " [options] [suite]...\n"
       << options.helpString() << "suite: run the specified test suite(s) only\n";
    return sb.str();
}

bool handlePreValidationTestFrameworkOptions(const moe::Environment& params,
                                             const std::vector<std::string>& args) {
    if (params.count("help")) {
        std::cout << getTestFrameworkHelp(args[0], moe::startupOptions) << std::endl;
        return false;
    }

    if (params.count("list")) {
        std::vector<std::string> suiteNames = mongo::unittest::getAllSuiteNames();
        for (std::vector<std::string>::const_iterator i = suiteNames.begin(); i != suiteNames.end();
             ++i) {
            std::cout << *i << std::endl;
        }
        return false;
    }

    return true;
}

Status storeTestFrameworkOptions(const moe::Environment& params,
                                 const std::vector<std::string>& args) {
    if (params.count("dbpath")) {
        frameworkGlobalParams.dbpathSpec = params["dbpath"].as<string>();
    }

    if (params.count("debug") || params.count("verbose")) {
        setMinimumLoggedSeverity(logv2::LogSeverity::Debug(1));
    }

    boost::filesystem::path p(frameworkGlobalParams.dbpathSpec);

    /* remove the contents of the test directory if it exists. */
    try {
        if (boost::filesystem::exists(p)) {
            if (!boost::filesystem::is_directory(p)) {
                StringBuilder sb;
                sb << "ERROR: path \"" << p.string() << "\" is not a directory";
                sb << getTestFrameworkHelp(args[0], moe::startupOptions);
                return Status(ErrorCodes::BadValue, sb.str());
            }
            boost::filesystem::directory_iterator end_iter;
            for (boost::filesystem::directory_iterator dir_iter(p); dir_iter != end_iter;
                 ++dir_iter) {
                boost::filesystem::remove_all(*dir_iter);
            }
        } else {
            boost::filesystem::create_directory(p);
        }
    } catch (const boost::filesystem::filesystem_error& e) {
        StringBuilder sb;
        sb << "boost::filesystem threw exception: " << e.what();
        return Status(ErrorCodes::BadValue, sb.str());
    }

    if (kDebugBuild)
        LOGV2(22491, "DEBUG build");

    string dbpathString = p.string();
    storageGlobalParams.dbpath = dbpathString.c_str();

    storageGlobalParams.engine = params["storage.engine"].as<string>();
    gFlowControlEnabled.store(params["enableFlowControl"].as<bool>());

    if (gFlowControlEnabled.load()) {
        LOGV2(22492, "Flow Control enabled");
    }

    if (params.count("replication.enableMajorityReadConcern")) {
        serverGlobalParams.enableMajorityReadConcern =
            params["replication.enableMajorityReadConcern"].as<bool>();
    }

    if (params.count("setParameter")) {
        std::map<std::string, std::string> parameters =
            params["setParameter"].as<std::map<std::string, std::string>>();
        auto* paramSet = ServerParameterSet::getNodeParameterSet();
        for (const auto& it : parameters) {
            auto parameter = paramSet->getIfExists(it.first);
            if (nullptr == parameter) {
                return {ErrorCodes::BadValue,
                        str::stream()
                            << "Illegal --setParameter parameter: \"" << it.first << "\""};
            }
            if (!parameter->allowedToChangeAtStartup()) {
                return {ErrorCodes::BadValue,
                        str::stream() << "Cannot use --setParameter to set \"" << it.first
                                      << "\" at startup"};
            }
            Status status = parameter->setFromString(it.second, boost::none);
            if (!status.isOK()) {
                return {ErrorCodes::BadValue,
                        str::stream() << "Bad value for parameter \"" << it.first
                                      << "\": " << status.reason()};
            }

            LOGV2(4539300,
                  "Setting server parameter",
                  "parameter"_attr = it.first,
                  "value"_attr = it.second);
        }
    }

    return Status::OK();
}
}  // namespace mongo
