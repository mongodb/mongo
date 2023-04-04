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

#include "mongo/db/matcher/schema/expression_internal_schema_str_length.h"

#include "mongo/bson/bsontypes.h"

namespace mongo {

InternalSchemaStrLengthMatchExpression::InternalSchemaStrLengthMatchExpression(
    MatchType type,
    boost::optional<StringData> path,
    long long strLen,
    StringData name,
    clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(type, path, std::move(annotation)), _name(name), _strLen(strLen) {}

void InternalSchemaStrLengthMatchExpression::debugString(StringBuilder& debug,
                                                         int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " " << _name << " " << _strLen;
    _debugStringAttachTagInfo(&debug);
}

BSONObj InternalSchemaStrLengthMatchExpression::getSerializedRightHandSide(
    SerializationOptions opts) const {
    BSONObjBuilder objBuilder;
    if (opts.replacementForLiteralArgs) {
        objBuilder.append(_name, opts.replacementForLiteralArgs.get());
    } else {
        objBuilder.append(_name, _strLen);
    }
    return objBuilder.obj();
}

bool InternalSchemaStrLengthMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const InternalSchemaStrLengthMatchExpression* realOther =
        static_cast<const InternalSchemaStrLengthMatchExpression*>(other);

    return path() == realOther->path() && _strLen == realOther->_strLen;
}

}  // namespace mongo
