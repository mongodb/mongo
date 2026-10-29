// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/util/assert_util.h"

#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

InternalSchemaFmodMatchExpression::InternalSchemaFmodMatchExpression(
    boost::optional<std::string_view> path,
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

void InternalSchemaFmodMatchExpression::debugString(StringBuilder& debug,
                                                    int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " fmod: divisor: " << _divisor.toString()
          << " remainder: " << _remainder.toString();
    _debugStringAttachTagInfo(&debug);
}

void InternalSchemaFmodMatchExpression::appendSerializedRightHandSide(
    BSONObjBuilder* bob, const query_shape::SerializationOptions& opts, bool includePath) const {
    bob->append("$_internalSchemaFmod"sv,
                BSON_ARRAY(opts.serializeLiteral(_divisor) << opts.serializeLiteral(_remainder)));
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
