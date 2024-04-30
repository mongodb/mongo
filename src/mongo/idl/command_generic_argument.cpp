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

#include "command_generic_argument.h"
#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/util/serialization_context.h"

namespace mongo {

bool isGenericArgument(StringData arg) {
    return GenericArguments::hasField(arg);
}

bool isGenericReply(StringData arg) {
    return GenericReplyFields::hasField(arg);
}

bool shouldForwardToShards(StringData arg) {
    return GenericArguments::shouldForwardToShards(arg);
}

bool shouldForwardFromShards(StringData replyField) {
    return GenericReplyFields::shouldForwardFromShards(replyField);
}

void appendGenericCommandArguments(const BSONObj& commandPassthroughFields,
                                   const std::vector<StringData>& knownFields,
                                   BSONObjBuilder* builder) {

    for (const auto& element : commandPassthroughFields) {

        StringData name = element.fieldNameStringData();
        // Include a passthrough field as long the IDL class has not defined it.
        if (isGenericArgument(name) &&
            std::find(knownFields.begin(), knownFields.end(), name) == knownFields.end()) {
            builder->append(element);
        }
    }
}

}  // namespace mongo
