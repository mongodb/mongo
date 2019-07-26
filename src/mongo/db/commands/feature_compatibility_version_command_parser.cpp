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

#include "mongo/db/commands/feature_compatibility_version_command_parser.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/command_generic_argument.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/commands/feature_compatibility_version_parser.h"

namespace mongo {

constexpr StringData FeatureCompatibilityVersionCommandParser::kCommandName;

StatusWith<std::string> FeatureCompatibilityVersionCommandParser::extractVersionFromCommand(
    StringData commandName, const BSONObj& cmdObj) {
    if (cmdObj.firstElementFieldName() != commandName) {
        return {ErrorCodes::InternalError,
                str::stream() << "Expected to find a " << commandName << " command, but found "
                              << cmdObj};
    }

    const auto versionElem = cmdObj.firstElement();

    if (versionElem.type() != BSONType::String) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "Command argument must be of type "
                                 "String, but was of type "
                              << typeName(versionElem.type()) << " in: " << cmdObj << ". See "
                              << feature_compatibility_version_documentation::kCompatibilityLink
                              << "."};
    }

    // Ensure that the command does not contain any unrecognized parameters
    for (const auto& cmdElem : cmdObj) {
        const auto fieldName = cmdElem.fieldNameStringData();
        if (fieldName == commandName || isGenericArgument(fieldName)) {
            continue;
        }

        uasserted(ErrorCodes::InvalidOptions,
                  str::stream() << "Unrecognized field found " << cmdElem.fieldNameStringData()
                                << " in " << cmdObj << ". See "
                                << feature_compatibility_version_documentation::kCompatibilityLink
                                << ".");
    }

    const std::string version = versionElem.String();

    if (version != FeatureCompatibilityVersionParser::kVersion42 &&
        version != FeatureCompatibilityVersionParser::kVersion40) {
        return {ErrorCodes::BadValue,
                str::stream() << "Invalid command argument. Expected '"
                              << FeatureCompatibilityVersionParser::kVersion42 << "' or '"
                              << FeatureCompatibilityVersionParser::kVersion40 << "', found "
                              << version << " in: " << cmdObj << ". See "
                              << feature_compatibility_version_documentation::kCompatibilityLink
                              << "."};
    }

    return version;
}

}  // namespace mongo
