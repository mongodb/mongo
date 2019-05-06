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

#include <boost/optional.hpp>
#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_merge_modes_gen.h"

namespace mongo {
class BSONObjBuilder;
class BSONElement;

// Defines a policy strategy describing what to do when there is a matching document in the target
// collection. Can hold a value from the MergeWhenMatchedModeEnum, or a custom pipeline definition.
struct MergeWhenMatchedPolicy {
    MergeWhenMatchedModeEnum mode;
    boost::optional<std::vector<BSONObj>> pipeline;
};

/**
 * Serialize and deserialize functions for the $merge stage 'into' field which can be a single
 * string value, or an object.
 */
void mergeTargetNssSerializeToBSON(const NamespaceString& targetNss,
                                   StringData fieldName,
                                   BSONObjBuilder* bob);
NamespaceString mergeTargetNssParseFromBSON(const BSONElement& elem);

/**
 * Serialize and deserialize functions for the $merge stage 'on' field which can be a single string
 * value, or array of strings.
 */
void mergeOnFieldsSerializeToBSON(const std::vector<std::string>& fields,
                                  StringData fieldName,
                                  BSONObjBuilder* bob);
std::vector<std::string> mergeOnFieldsParseFromBSON(const BSONElement& elem);

/**
 * Serialize and deserialize functions for the $merge stage 'whenMatched' field which can be either
 * a string value, or an array of objects defining a custom pipeline.
 */
void mergeWhenMatchedSerializeToBSON(const MergeWhenMatchedPolicy& policy,
                                     StringData fieldName,
                                     BSONObjBuilder* bob);
MergeWhenMatchedPolicy mergeWhenMatchedParseFromBSON(const BSONElement& elem);
}  // namespace mongo
