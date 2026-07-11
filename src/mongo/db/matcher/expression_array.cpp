// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/expression_array.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/query/util/make_data_structure.h"

#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

bool ArrayMatchingMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const ArrayMatchingMatchExpression* realOther =
        static_cast<const ArrayMatchingMatchExpression*>(other);

    if (path() != realOther->path())
        return false;

    if (numChildren() != realOther->numChildren())
        return false;

    for (unsigned i = 0; i < numChildren(); i++)
        if (!getChild(i)->equivalent(realOther->getChild(i)))
            return false;
    return true;
}


// -------

ElemMatchObjectMatchExpression::ElemMatchObjectMatchExpression(
    boost::optional<std::string_view> path,
    std::unique_ptr<MatchExpression> sub,
    clonable_ptr<ErrorAnnotation> annotation)
    : ArrayMatchingMatchExpression(ELEM_MATCH_OBJECT, path, std::move(annotation)),
      _sub(std::move(sub)) {}

void ElemMatchObjectMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " $elemMatch (obj)";
    _debugStringAttachTagInfo(&debug);
    _sub->debugString(debug, indentationLevel + 1);
}

void ElemMatchObjectMatchExpression::appendSerializedRightHandSide(
    BSONObjBuilder* bob, const query_shape::SerializationOptions& opts, bool includePath) const {
    BSONObjBuilder elemMatchBob = bob->subobjStart("$elemMatch");
    query_shape::SerializationOptions options = opts;
    _sub->serialize(&elemMatchBob, options, true);
    elemMatchBob.doneFast();
}

// -------

ElemMatchValueMatchExpression::ElemMatchValueMatchExpression(
    boost::optional<std::string_view> path,
    std::unique_ptr<MatchExpression> sub,
    clonable_ptr<ErrorAnnotation> annotation)
    : ArrayMatchingMatchExpression(ELEM_MATCH_VALUE, path, std::move(annotation)),
      _subs(makeVector(std::move(sub))) {}

ElemMatchValueMatchExpression::ElemMatchValueMatchExpression(
    boost::optional<std::string_view> path, clonable_ptr<ErrorAnnotation> annotation)
    : ArrayMatchingMatchExpression(ELEM_MATCH_VALUE, path, std::move(annotation)) {}

void ElemMatchValueMatchExpression::add(std::unique_ptr<MatchExpression> sub) {
    _subs.push_back(std::move(sub));
}

void ElemMatchValueMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " $elemMatch (value)";

    _debugStringAttachTagInfo(&debug);

    for (unsigned i = 0; i < _subs.size(); i++) {
        _subs[i]->debugString(debug, indentationLevel + 1);
    }
}

void ElemMatchValueMatchExpression::appendSerializedRightHandSide(
    BSONObjBuilder* bob, const query_shape::SerializationOptions& opts, bool includePath) const {
    BSONObjBuilder emBob = bob->subobjStart("$elemMatch");
    query_shape::SerializationOptions options = opts;
    for (auto&& child : _subs) {
        child->serialize(&emBob, options, false);
    }
    emBob.doneFast();
}

// ---------

SizeMatchExpression::SizeMatchExpression(boost::optional<std::string_view> path,
                                         int size,
                                         clonable_ptr<ErrorAnnotation> annotation)
    : ArrayMatchingMatchExpression(SIZE, path, std::move(annotation)), _size(size) {}

void SizeMatchExpression::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " $size : " << _size;

    _debugStringAttachTagInfo(&debug);
}

void SizeMatchExpression::appendSerializedRightHandSide(
    BSONObjBuilder* bob, const query_shape::SerializationOptions& opts, bool includePath) const {
    opts.appendLiteral(bob, "$size", _size);
}

bool SizeMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType())
        return false;

    const SizeMatchExpression* realOther = static_cast<const SizeMatchExpression*>(other);
    return path() == realOther->path() && _size == realOther->_size;
}


// ------------------
}  // namespace mongo
