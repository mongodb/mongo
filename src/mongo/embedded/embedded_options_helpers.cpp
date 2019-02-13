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

#include "mongo/embedded/embedded_options_helpers.h"

#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/startup_options.h"

#include <iterator>
#include <map>
#include <vector>

namespace mongo {
namespace embedded_integration_helpers {

Status parseCommandLineOptions(int argc,
                               char* argv[],
                               const optionenvironment::OptionSection& startupOptions) {
    // We manually run the options parsing code that's equivalent to the logic in the
    // MONGO_INITIALIZERs for mongod. We cannot do this in initializers because embedded uses a
    // different options format and we therefore need to have parsed the command line options before
    // embedded::initialize() is called. However, as long as we store the options in the same place
    // they will be valid for embedded too.
    std::vector<std::string> args;
    std::map<std::string, std::string> env;

    args.reserve(argc);
    std::copy(argv, argv + argc, std::back_inserter(args));

    optionenvironment::OptionsParser parser;
    return parser.run(startupOptions, args, env, &optionenvironment::startupOptionsParsed);
}

}  // namespace embedded_integration_helpers
}  // namepsace mongo
