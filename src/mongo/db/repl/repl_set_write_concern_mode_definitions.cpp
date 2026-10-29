// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/repl_set_write_concern_mode_definitions.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <string_view>

#include <absl/container/flat_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>

namespace mongo {
namespace repl {
void ReplSetWriteConcernModeDefinitions::serializeToBSON(std::string_view fieldName,
                                                         BSONObjBuilder* bob) const {
    BSONObjBuilder mapBuilder(bob->subobjStart(fieldName));
    std::vector<std::pair<std::string_view, const Definition*>> sortedDefinitions;
    for (const auto& definitionItems : _definitions) {
        sortedDefinitions.emplace_back(definitionItems.first, &definitionItems.second);
    }
    std::sort(sortedDefinitions.begin(), sortedDefinitions.end());
    for (const auto& definitionItems : sortedDefinitions) {
        BSONObjBuilder defBuilder(mapBuilder.subobjStart(definitionItems.first));
        for (const auto& constraint : *definitionItems.second) {
            defBuilder.append(constraint.first, constraint.second);
        }
        defBuilder.done();
    }
    mapBuilder.done();
}

/* static */
ReplSetWriteConcernModeDefinitions ReplSetWriteConcernModeDefinitions::parseFromBSON(
    BSONElement patternMapElement) {
    const auto& fieldName = patternMapElement.fieldName();
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "Expected " << fieldName << " to be an Object, it actually had type "
                          << typeName(patternMapElement.type()),
            patternMapElement.type() == BSONType::object);
    Definitions definitions;
    BSONObj modes = patternMapElement.Obj();
    for (auto&& modeElement : modes) {
        uassert(51001,
                str::stream() << fieldName << " contains multiple fields named "
                              << modeElement.fieldName(),
                definitions.find(modeElement.fieldNameStringData()) == definitions.end());
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "Expected " << fieldName << '.' << modeElement.fieldName()
                              << " to be an Object, not " << typeName(modeElement.type()),
                modeElement.type() == BSONType::object);
        Definition& definition = definitions[modeElement.fieldNameStringData()];
        for (auto&& constraintElement : modeElement.Obj()) {
            uassert(ErrorCodes::TypeMismatch,
                    str::stream() << "Expected " << fieldName << '.' << modeElement.fieldName()
                                  << '.' << constraintElement.fieldName() << " to be a number, not "
                                  << typeName(constraintElement.type()),
                    constraintElement.isNumber());
            const int minCount = constraintElement.numberInt();
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Value of " << fieldName << '.' << modeElement.fieldName()
                                  << '.' << constraintElement.fieldName()
                                  << " must be positive, but found " << minCount,
                    minCount > 0);
            definition.emplace_back(constraintElement.fieldNameStringData(), minCount);
        }
    }
    return ReplSetWriteConcernModeDefinitions(std::move(definitions));
}

StatusWith<StringMap<ReplSetTagPattern>> ReplSetWriteConcernModeDefinitions::convertToTagPatternMap(
    ReplSetTagConfig* tagConfig) const {
    StringMap<ReplSetTagPattern> result;
    for (const auto& definitionItems : _definitions) {
        ReplSetTagPattern& pattern =
            result.insert({definitionItems.first, tagConfig->makePattern()}).first->second;
        for (const auto& constraint : definitionItems.second) {
            Status status = tagConfig->addTagCountConstraintToPattern(
                &pattern, constraint.first, constraint.second);
            if (!status.isOK()) {
                return status;
            }
        }
    }
    return result;
}

}  // namespace repl
}  // namespace mongo
