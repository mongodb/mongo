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

#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

InternalSchemaFmodMatchExpression::InternalSchemaFmodMatchExpression(
    boost::optional<StringData> path,
    Decimal128 divisor,
    Decimal128 remainder,
    clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(MatchType::INTERNAL_SCHEMA_FMOD, path, std::move(annotation)),
      _divisor(divisor),
      _remainder(remainder) {
    uassert(ErrorCodes::BadValue, "divisor cannot be 0", !divisor.isZero());
    uassert(ErrorCodes::BadValue, "divisor cannot be NaN", !divisor.isNaN());
    uassert(ErrorCodes::BadValue, "divisor cannot be infinite", !divisor.isInfinite());
}

bool InternalSchemaFmodMatchExpression::matchesSingleElement(const BSONElement& e,
                                                             MatchDetails* details) const {
    if (!e.isNumber()) {
        return false;
    }
    std::uint32_t flags = Decimal128::SignalingFlag::kNoFlag;
    Decimal128 result = e.numberDecimal().modulo(_divisor, &flags);
    if (flags == Decimal128::SignalingFlag::kNoFlag) {
        return result.isEqual(_remainder);
    }
    return false;
}

void InternalSchemaFmodMatchExpression::debugString(StringBuilder& debug,
                                                    int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " fmod: divisor: " << _divisor.toString()
          << " remainder: " << _remainder.toString();
    _debugStringAttachTagInfo(&debug);
}

BSONObj InternalSchemaFmodMatchExpression::getSerializedRightHandSide(
    SerializationOptions opts) const {
    return BSON("$_internalSchemaFmod"_sd << BSON_ARRAY(opts.serializeLiteral(_divisor)
                                                        << opts.serializeLiteral(_remainder)));
}

bool InternalSchemaFmodMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType()) {
        return false;
    }

    const InternalSchemaFmodMatchExpression* realOther =
        static_cast<const InternalSchemaFmodMatchExpression*>(other);
    return path() == realOther->path() && _divisor.isEqual(realOther->_divisor) &&
        _remainder.isEqual(realOther->_remainder);
}

}  //  namespace mongo
