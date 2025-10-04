/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"

#include <string>

namespace mongo {

namespace optionenvironment {
class Environment;
}  // namespace optionenvironment

/**
 * Base server options that are available in all applications, standalone and embedded.
 *
 * Included by addGeneralServerOptions, don't call both.
 */
Status addBaseServerOptions(optionenvironment::OptionSection*);

/**
 * General server options for most standalone applications. Includes addBaseServerOptions.
 */
Status addGeneralServerOptions(optionenvironment::OptionSection*);

Status validateSystemLogDestinationSetting(const std::string&);
Status validateSecurityClusterAuthModeSetting(const std::string&);
Status canonicalizeNetBindIpAll(optionenvironment::Environment*);

std::string getUnixDomainSocketFilePermissionsHelpText();

}  // namespace mongo
