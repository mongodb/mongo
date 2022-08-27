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

#include <map>
#include <string>

#include "mongo/base/status.h"
#include "mongo/db/server_parameter.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"

namespace mongo {

namespace optionenvironment {
class OptionSection;
class Environment;
}  // namespace optionenvironment

namespace moe = mongo::optionenvironment;

namespace server_options_detail {
StatusWith<BSONObj> applySetParameterOptions(const std::map<std::string, std::string>& toApply,
                                             ServerParameterSet& parameterSet);
}  // namespace server_options_detail

/**
 * Handle custom validation of base options that can not currently be done by using
 * Constraints in the Environment.  See the "validate" function in the Environment class for
 * more details.
 */
Status validateBaseOptions(const moe::Environment& params);

/**
 * Canonicalize base options for the given environment.
 *
 * For example, the options "objcheck", "noobjcheck", and "net.wireObjectCheck" should all be
 * merged into "net.wireObjectCheck".
 */
Status canonicalizeBaseOptions(moe::Environment* params);

/**
 * Sets up the global server state necessary to be able to store the server options, based on how
 * the server was started.
 *
 * For example, saves the current working directory in serverGlobalParams.cwd so that relative paths
 * in server options can be interpreted correctly.
 */
Status setupBaseOptions(const std::vector<std::string>& args);

/**
 * Store the given parsed params in global server state.
 *
 * For example, sets the serverGlobalParams.quiet variable based on the systemLog.quiet config
 * parameter.
 */
Status storeBaseOptions(const moe::Environment& params);

}  // namespace mongo
