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

#include "mongo/util/options_parser/startup_options.h"

#include <iostream>

#include "mongo/util/exit_code.h"
#include "mongo/util/options_parser/option_description.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/quick_exit.h"

namespace mongo {
namespace optionenvironment {
namespace {

MONGO_STARTUP_OPTIONS_PARSE(StartupOptions)(InitializerContext* context) {
    OptionsParser parser;
    Status ret = parser.run(startupOptions, context->args(), &startupOptionsParsed);
    if (!ret.isOK()) {
        std::cerr << ret.reason() << std::endl;
        // TODO: Figure out if there's a use case for this help message ever being different
        std::cerr << "try '" << context->args()[0] << " --help' for more information" << std::endl;
        quickExit(ExitCode::badOptions);
    }
}

MONGO_INITIALIZER_GENERAL(OutputConfig,
                          ("EndStartupOptionValidation"),
                          ("BeginStartupOptionStorage"))
(InitializerContext*) {
    if (startupOptionsParsed.count("outputConfig")) {
        bool output = false;
        uassertStatusOK(startupOptionsParsed.get(Key("outputConfig"), &output));
        if (output) {
            std::cout << startupOptionsParsed.toYAML() << std::endl;
            quickExit(ExitCode::clean);
        }
    }
}

}  // namespace
}  // namespace optionenvironment
}  // namespace mongo
