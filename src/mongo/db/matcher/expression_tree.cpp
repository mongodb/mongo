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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_path.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/query/collation/collator_interface.h"

#include <algorithm>
#include <iterator>
#include <string>

namespace mongo {
namespace {

PathMatchExpression* getEligiblePathMatchForNotSerialization(MatchExpression* expr) {
    // Returns a pointer to a PathMatchExpression if 'expr' is such a pointer, otherwise returns
    // nullptr.
    //
    // One exception: while TextMatchExpressionBase derives from PathMatchExpression, text match
    // expressions cannot be serialized in the same manner as other PathMatchExpression derivatives.
    // This is because the path for a TextMatchExpression is embedded within the $text object,
    // whereas for other PathMatchExpressions it is on the left-hand-side, for example {x: {$eq:
    // 1}}.
    //
    // Rather than the following dynamic_cast, we'll do a more performant, but also more verbose
    // check.
    // dynamic_cast<PathMatchExpression*>(expr) && !dynamic_cast<TextMatchExpressionBase*>(expr)
    //
    // This version below is less obviously exhaustive, but because this is just a legibility
    // optimization, and this function also gets called on the query shape stats recording hot path,
    // we think it is worth it.
    //
    // This function will return nullptr if the field path contains special characters like a "$"
    // prefix. We will serialize to a $nor and delegate the path serialization to lower in the tree,
    // which has special handlers for those characters.
    if (expr->path().starts_with('$')) {
        return nullptr;
    }

    switch (expr->matchType()) {
        // leaf types
        case MatchExpression::EQ:
        case MatchExpression::LTE:
        case MatchExpression::LT:
        case MatchExpression::GT:
        case MatchExpression::GTE:
        case MatchExpression::REGEX:
        case MatchExpression::MOD:
        case MatchExpression::EXISTS:
        case MatchExpression::MATCH_IN:
        case MatchExpression::BITS_ALL_SET:
        case MatchExpression::BITS_ALL_CLEAR:
        case MatchExpression::BITS_ANY_SET:
        case MatchExpression::BITS_ANY_CLEAR:
        // array types
        case MatchExpression::ELEM_MATCH_OBJECT:
        case MatchExpression::ELEM_MATCH_VALUE:
        case MatchExpression::SIZE:
        // special types
        case MatchExpression::TYPE_OPERATOR:
        case MatchExpression::GEO:
        case MatchExpression::GEO_NEAR:
        // Internal subclasses of PathMatchExpression:
        case MatchExpression::INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX:
        case MatchExpression::INTERNAL_SCHEMA_BIN_DATA_ENCRYPTED_TYPE:
        case MatchExpression::INTERNAL_SCHEMA_BIN_DATA_FLE2_ENCRYPTED_TYPE:
        case MatchExpression::INTERNAL_SCHEMA_BIN_DATA_SUBTYPE:
        case MatchExpression::INTERNAL_SCHEMA_MATCH_ARRAY_INDEX:
        case MatchExpression::INTERNAL_SCHEMA_MAX_ITEMS:
        case MatchExpression::INTERNAL_SCHEMA_MAX_LENGTH:
        case MatchExpression::INTERNAL_SCHEMA_MAX_PROPERTIES:
        case MatchExpression::INTERNAL_SCHEMA_MIN_ITEMS:
        case MatchExpression::INTERNAL_SCHEMA_MIN_LENGTH:
        case MatchExpression::INTERNAL_SCHEMA_TYPE:
        case MatchExpression::INTERNAL_SCHEMA_UNIQUE_ITEMS:
            return static_cast<PathMatchExpression*>(expr);
        // purposefully skip TEXT:
        case MatchExpression::TEXT:
        // Any other type is not considered a PathMatchExpression.
        case MatchExpression::AND:
        case MatchExpression::OR:
        case MatchExpression::NOT:
        case MatchExpression::NOR:
        case MatchExpression::WHERE:
        case MatchExpression::EXPRESSION:
        case MatchExpression::ALWAYS_FALSE:
        case MatchExpression::ALWAYS_TRUE:
        case MatchExpression::INTERNAL_2D_POINT_IN_ANNULUS:
        case MatchExpression::INTERNAL_BUCKET_GEO_WITHIN:
        case MatchExpression::INTERNAL_EXPR_EQ:
        case MatchExpression::INTERNAL_EXPR_GT:
        case MatchExpression::INTERNAL_EXPR_GTE:
        case MatchExpression::INTERNAL_EXPR_LT:
        case MatchExpression::INTERNAL_EXPR_LTE:
        case MatchExpression::INTERNAL_EQ_HASHED_KEY:
        case MatchExpression::INTERNAL_SCHEMA_ALLOWED_PROPERTIES:
        case MatchExpression::INTERNAL_SCHEMA_COND:
        case MatchExpression::INTERNAL_SCHEMA_EQ:
        case MatchExpression::INTERNAL_SCHEMA_FMOD:
        case MatchExpression::INTERNAL_SCHEMA_MIN_PROPERTIES:
        case MatchExpression::INTERNAL_SCHEMA_OBJECT_MATCH:
        case MatchExpression::INTERNAL_SCHEMA_ROOT_DOC_EQ:
        case MatchExpression::INTERNAL_SCHEMA_XOR:
            return nullptr;
        default:
            MONGO_UNREACHABLE_TASSERT(7800300);
    }
};
}  // namespace

void ListOfMatchExpression::_debugList(StringBuilder& debug, int indentationLevel) const {
    for (unsigned i = 0; i < _expressions.size(); i++) {
        _expressions[i]->debugString(debug, indentationLevel + 1);
    }
}

void ListOfMatchExpression::_listToBSON(BSONArrayBuilder* out,
                                        const SerializationOptions& opts,
                                        bool includePath) const {
    for (unsigned i = 0; i < _expressions.size(); i++) {
        BSONObjBuilder childBob(out->subobjStart());
        _expressions[i]->serialize(&childBob, opts, includePath);
    }
    out->doneFast();
}

bool ListOfMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType()) {
        return false;
    }
    const ListOfMatchExpression* realOther = static_cast<const ListOfMatchExpression*>(other);

    if (_expressions.size() != realOther->_expressions.size()) {
        return false;
    }
    // TOOD: order doesn't matter
    for (unsigned i = 0; i < _expressions.size(); i++) {
        if (!_expressions[i]->equivalent(realOther->_expressions[i].get())) {
            return false;
        }
    }
    return true;
}

void AndMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << "$and";
    _debugStringAttachTagInfo(&debug);
    _debugList(debug, indentationLevel);
}

void AndMatchExpression::serialize(BSONObjBuilder* out,
                                   const SerializationOptions& opts,
                                   bool includePath) const {
    if (!numChildren()) {
        // It is possible for an AndMatchExpression to have no children, resulting in the serialized
        // expression {$and: []}, which is not a valid query object.
        return;
    }

    BSONArrayBuilder arrBob(out->subarrayStart("$and"));
    _listToBSON(&arrBob, opts, includePath);
    arrBob.doneFast();
}

bool AndMatchExpression::isTriviallyTrue() const {
    return numChildren() == 0;
}

// -----

void OrMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << "$or";
    _debugStringAttachTagInfo(&debug);
    _debugList(debug, indentationLevel);
}

void OrMatchExpression::serialize(BSONObjBuilder* out,
                                  const SerializationOptions& opts,
                                  bool includePath) const {
    if (!numChildren()) {
        // It is possible for an OrMatchExpression to have no children, resulting in the serialized
        // expression {$or: []}, which is not a valid query object. An empty $or is logically
        // equivalent to {$alwaysFalse: 1}.
        out->append(AlwaysFalseMatchExpression::kName, 1);
        return;
    }
    BSONArrayBuilder arrBob(out->subarrayStart("$or"));
    _listToBSON(&arrBob, opts, includePath);
}

bool OrMatchExpression::isTriviallyFalse() const {
    return numChildren() == 0;
}

// ----

void NorMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << "$nor";
    _debugStringAttachTagInfo(&debug);
    _debugList(debug, indentationLevel);
}

void NorMatchExpression::serialize(BSONObjBuilder* out,
                                   const SerializationOptions& opts,
                                   bool includePath) const {
    BSONArrayBuilder arrBob(out->subarrayStart("$nor"));
    _listToBSON(&arrBob, opts, includePath);
}

// -------

void NotMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << "$not";
    _debugStringAttachTagInfo(&debug);
    _exp->debugString(debug, indentationLevel + 1);
}

void NotMatchExpression::serializeNotExpressionToNor(MatchExpression* exp,
                                                     BSONObjBuilder* out,
                                                     const SerializationOptions& opts,
                                                     bool includePath) {
    BSONObjBuilder childBob;
    exp->serialize(&childBob, opts, includePath);
    BSONObj tempObj = childBob.obj();

    BSONArrayBuilder tBob(out->subarrayStart("$nor"));
    tBob.append(tempObj);
    tBob.doneFast();
}

void NotMatchExpression::serialize(BSONObjBuilder* out,
                                   const SerializationOptions& opts,
                                   bool includePath) const {
    if (_exp->matchType() == MatchType::AND && _exp->numChildren() == 0) {
        opts.appendLiteral(out, "$alwaysFalse", 1);
        return;
    }

    if (!includePath) {
        BSONObjBuilder notBob(out->subobjStart("$not"));
        // Our parser does not accept a $and directly within a $not, instead expecting the direct
        // notation like {x: {$not: {$gt: 5, $lt: 0}}}. We represent such an expression with an AND
        // internally, so we un-nest it here to be able to re-parse it.
        if (_exp->matchType() == MatchType::AND) {
            for (size_t x = 0; x < _exp->numChildren(); ++x) {
                _exp->getChild(x)->serialize(&notBob, opts, includePath);
            }
        } else {
            _exp->serialize(&notBob, opts, includePath);
        }
        return;
    }

    auto expressionToNegate = _exp.get();
    if (_exp->matchType() == MatchType::AND && _exp->numChildren() == 1) {
        expressionToNegate = _exp->getChild(0);
    }

    // It is generally easier to be correct if we just always serialize to a $nor, since this will
    // delegate the path serialization to lower in the tree where we have the information on-hand.
    // However, for legibility we preserve a $not with a single path-accepting child as a $not.
    if (auto pathMatch = getEligiblePathMatchForNotSerialization(expressionToNegate)) {
        auto append = [&](StringData path) {
            BSONObjBuilder pathBob(out->subobjStart(path));
            pathBob.append("$not", pathMatch->getSerializedRightHandSide(opts));
        };
        append(opts.serializeFieldPathFromString(pathMatch->path()));
        return;
    }
    return serializeNotExpressionToNor(expressionToNegate, out, opts);
}

bool NotMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    return _exp->equivalent(other->getChild(0));
}

}  // namespace mongo
