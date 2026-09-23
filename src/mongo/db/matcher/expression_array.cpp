// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/expression_array.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/query/util/make_data_structure.h"

#include <algorithm>
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
    _sub->serialize(&elemMatchBob, opts, true);
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
    // A body that imposes no condition on the elements matches any element, i.e. it means "this
    // field is a non-empty array".
    const bool bodyIsAlwaysTrue = std::all_of(
        _subs.begin(), _subs.end(), [](const auto& child) { return child->isTriviallyTrue(); });
    if (bodyIsAlwaysTrue) {
        // Encode the trivially-true body as {$elemMatch: {$nin: []}}, instead of {$elemMatch:
        // {}}. {$elemMatch: {$nin: []}} is parsed into ElemMatchValue with exactly the same
        // semantics, whereas {$elemMatch: {}} is parsed into ElemMatchObject with an empty $and,
        // which requires each element to be either an object or an array.
        //
        // Two other encodings were considered but not used:
        //   - {$elemMatch: {$alwaysTrue: 1}} is parsed into ElemMatchObject, not the value form.
        //   - {$elemMatch: {$exists: true}} is parsed into ElemMatchValue, but no optimizer
        //     rewrite folds the resulting empty-path Exists back into the empty $and body, so
        //     the re-parsed and re-optimized tree is not structurally equivalent to the original
        //     shape, even though both match the same documents.
        BSONArrayBuilder arrBob;
        opts.appendLiteral(&emBob, "$nin", arrBob.arr());
    } else {
        for (auto&& child : _subs) {
            child->serialize(&emBob, opts, false);
        }
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
