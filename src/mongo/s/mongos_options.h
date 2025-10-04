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

#pragma once

#include "mongo/base/status.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/server_options.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/value.h"

#include <string>
#include <vector>

namespace mongo {

namespace optionenvironment {
class OptionSection;
class Environment;
}  // namespace optionenvironment

namespace moe = mongo::optionenvironment;

struct MongosGlobalParams {
    bool scriptingEnabled = true;  // Use "security.javascriptEnabled" to set this variable. Or use
                                   // --noscripting which will set it to false.

    // The config server connection string
    ConnectionString configdbs;
};

extern MongosGlobalParams mongosGlobalParams;

void printMongosHelp(const moe::OptionSection& options);

/**
 * Handle options that should come before validation, such as "help".
 *
 * Returns false if an option was found that implies we should prematurely exit with success.
 */
bool handlePreValidationMongosOptions(const moe::Environment& params,
                                      const std::vector<std::string>& args);

/**
 * Handle custom validation of mongos options that can not currently be done by using
 * Constraints in the Environment.  See the "validate" function in the Environment class for
 * more details.
 */
Status validateMongosOptions(const moe::Environment& params);

/**
 * Canonicalize mongos options for the given environment.
 *
 * For example, the options "noscripting" and "security.javascriptEnabled" and should all be merged
 * into "security.javascriptEnabled".
 */
Status canonicalizeMongosOptions(moe::Environment* params);

Status storeMongosOptions(const moe::Environment& params);
}  // namespace mongo
