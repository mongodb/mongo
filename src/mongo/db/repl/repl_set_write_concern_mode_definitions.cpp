/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/repl/repl_set_write_concern_mode_definitions.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
void ReplSetWriteConcernModeDefinitions::serializeToBSON(StringData fieldName,
                                                         BSONObjBuilder* bob) const {
    BSONObjBuilder mapBuilder(bob->subobjStart(fieldName));
    for (const auto& definitionItems : _definitions) {
        BSONObjBuilder defBuilder(mapBuilder.subobjStart(definitionItems.first));
        for (const auto& constraint : definitionItems.second) {
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
            patternMapElement.type() == Object);
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
                modeElement.type() == Object);
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
