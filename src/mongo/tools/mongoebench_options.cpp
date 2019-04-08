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

#include "mongo/tools/mongoebench_options.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>

#include "mongo/base/status.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/platform/random.h"
#include "mongo/shell/bench.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/str.h"

namespace mongo {

MongoeBenchGlobalParams mongoeBenchGlobalParams;

void printMongoeBenchHelp(std::ostream* out) {
    *out << "Usage: mongoebench <config file> [options]" << std::endl;
    *out << moe::startupOptions.helpString();
    *out << std::flush;
}

bool handlePreValidationMongoeBenchOptions(const moe::Environment& params) {
    if (params.count("help")) {
        printMongoeBenchHelp(&std::cout);
        return false;
    }
    return true;
}

namespace {

BSONObj getBsonFromJsonFile(const std::string& filename) {
    std::ifstream infile(filename.c_str());
    std::string data((std::istreambuf_iterator<char>(infile)), std::istreambuf_iterator<char>());
    return fromjson(data);
}

boost::filesystem::path kDefaultOutputFile("perf.json");

}  // namespace

Status storeMongoeBenchOptions(const moe::Environment& params,
                               const std::vector<std::string>& args) {
    if (!params.count("benchRunConfigFile")) {
        return {ErrorCodes::BadValue, "No benchRun config file was specified"};
    }

    BSONObj config = getBsonFromJsonFile(params["benchRunConfigFile"].as<std::string>());
    for (auto&& elem : config) {
        const auto fieldName = elem.fieldNameStringData();
        if (fieldName == "pre") {
            mongoeBenchGlobalParams.preConfig.reset(
                BenchRunConfig::createFromBson(elem.wrap("ops")));
        } else if (fieldName == "ops") {
            mongoeBenchGlobalParams.opsConfig.reset(BenchRunConfig::createFromBson(elem.wrap()));
        } else {
            return {ErrorCodes::BadValue,
                    str::stream() << "Unrecognized key in benchRun config file: " << fieldName};
        }
    }

    int64_t seed = params.count("seed") ? static_cast<int64_t>(params["seed"].as<long>())
                                        : SecureRandom::create()->nextInt64();

    if (mongoeBenchGlobalParams.preConfig) {
        mongoeBenchGlobalParams.preConfig->randomSeed = seed;
    }

    if (mongoeBenchGlobalParams.opsConfig) {
        mongoeBenchGlobalParams.opsConfig->randomSeed = seed;

        if (params.count("threads")) {
            mongoeBenchGlobalParams.opsConfig->parallel = params["threads"].as<unsigned>();
        }

        if (params.count("time")) {
            mongoeBenchGlobalParams.opsConfig->seconds = params["time"].as<double>();
        }
    }

    if (params.count("output")) {
        mongoeBenchGlobalParams.outputFile =
            boost::filesystem::path(params["output"].as<std::string>());
    } else {
        boost::filesystem::path dbpath(storageGlobalParams.dbpath);
        mongoeBenchGlobalParams.outputFile = dbpath / kDefaultOutputFile;
    }

    mongoeBenchGlobalParams.outputFile = mongoeBenchGlobalParams.outputFile.lexically_normal();
    auto parentPath = mongoeBenchGlobalParams.outputFile.parent_path();
    if (!parentPath.empty() && !boost::filesystem::exists(parentPath)) {
        return {ErrorCodes::NonExistentPath,
                str::stream() << "Directory containing output file must already exist, but "
                              << parentPath.string()
                              << " wasn't found"};
    }

    return Status::OK();
}

}  // namespace mongo
