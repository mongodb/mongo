/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/commands/feature_compatibility_version_command_parser.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/query_request.h"
#include "mongo/util/version.h"

namespace mongo {
namespace {
constexpr StringData kVersion32 = "3.2"_sd;
}  // namespace

constexpr StringData FeatureCompatibilityVersionCommandParser::kVersion34;
constexpr StringData FeatureCompatibilityVersionCommandParser::kVersion36;
constexpr StringData FeatureCompatibilityVersionCommandParser::kVersionUpgradingTo36;
constexpr StringData FeatureCompatibilityVersionCommandParser::kVersionDowngradingTo34;
constexpr StringData FeatureCompatibilityVersionCommandParser::kVersionUnset;

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
                              << typeName(versionElem.type())
                              << " in: "
                              << cmdObj
                              << ". See "
                              << feature_compatibility_version::kDochubLink
                              << "."};
    }

    // Ensure that the command does not contain any unrecognized parameters
    for (const auto& cmdElem : cmdObj) {
        const auto fieldName = cmdElem.fieldNameStringData();
        if (fieldName == commandName || Command::isGenericArgument(fieldName)) {
            continue;
        }

        uasserted(ErrorCodes::InvalidOptions,
                  str::stream() << "Unrecognized field found " << cmdElem.fieldNameStringData()
                                << " in "
                                << cmdObj
                                << ". See "
                                << feature_compatibility_version::kDochubLink
                                << ".");
    }

    const std::string version = versionElem.String();

    if (version == kVersion32) {
        return {ErrorCodes::BadValue,
                str::stream() << "Invalid command argument: '" << kVersion32
                              << "'. You must downgrade to MongoDB 3.4 to enable "
                                 "featureCompatibilityVersion 3.2. See "
                              << feature_compatibility_version::kDochubLink
                              << "."};
    }

    if (version != FeatureCompatibilityVersionCommandParser::kVersion36 &&
        version != FeatureCompatibilityVersionCommandParser::kVersion34) {
        return {ErrorCodes::BadValue,
                str::stream() << "Invalid command argument. Expected '"
                              << FeatureCompatibilityVersionCommandParser::kVersion36
                              << "' or '"
                              << FeatureCompatibilityVersionCommandParser::kVersion34
                              << "', found "
                              << version
                              << " in: "
                              << cmdObj
                              << ". See "
                              << feature_compatibility_version::kDochubLink
                              << "."};
    }

    return version;
}

}  // namespace mongo
