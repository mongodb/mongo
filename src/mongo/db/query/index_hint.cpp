/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/index_hint.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/str.h"

#include <boost/functional/hash.hpp>

namespace mongo {
namespace {
static constexpr auto kNaturalFieldName = "$natural"_sd;

std::strong_ordering compare(const IndexKeyPattern& a, const IndexKeyPattern& b) {
    return a.woCompare(b) <=> 0;
}

std::strong_ordering compare(const IndexName& a, const IndexName& b) {
    // NOTE: spaceship operator is not available on macos for strings, therefore implementing one
    // myself instead.
    return a.compare(b) <=> 0;
}

std::strong_ordering compare(const NaturalOrderHint& a, const NaturalOrderHint& b) {
    return a <=> b;
}
};  // namespace

bool isForward(NaturalOrderHint::Direction dir) {
    return dir == NaturalOrderHint::Direction::kForward;
}

std::string toString(NaturalOrderHint::Direction dir) {
    StackStringBuilder ssb;
    ssb << dir;
    return ssb.str();
}

IndexHint IndexHint::parse(const BSONElement& element) {
    if (element.type() == BSONType::string) {
        return IndexHint(element.String());
    } else if (element.type() == BSONType::object) {
        auto obj = element.Obj();
        if (obj.firstElementFieldName() == kNaturalFieldName) {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "$natural hint may only accept one field"
                                  << element.toString(),
                    obj.nFields() == 1);
            switch (obj.firstElement().numberInt()) {
                case 1:
                    return IndexHint(NaturalOrderHint(NaturalOrderHint::Direction::kForward));
                case -1:
                    return IndexHint(NaturalOrderHint(NaturalOrderHint::Direction::kBackward));
                default:
                    uasserted(ErrorCodes::FailedToParse,
                              str::stream() << "$natural hint may only accept 1 or -1, not "
                                            << element.toString());
            }
        }
        return IndexHint(obj.getOwned());
    } else {
        uasserted(ErrorCodes::FailedToParse, "Hint must be a string or an object");
    }
}

void IndexHint::append(const IndexHint& hint, StringData fieldName, BSONObjBuilder* builder) {
    visit(OverloadedVisitor{
              [&](const IndexKeyPattern& keyPattern) { builder->append(fieldName, keyPattern); },
              [&](const IndexName& indexName) { builder->append(fieldName, indexName); },
              [&](const NaturalOrderHint& naturalOrderHint) {
                  builder->append(fieldName, BSON(kNaturalFieldName << naturalOrderHint.direction));
              }},
          hint._hint);
}

void IndexHint::append(BSONArrayBuilder* builder) const {
    visit(OverloadedVisitor{[&](const IndexKeyPattern& keyPattern) { builder->append(keyPattern); },
                            [&](const IndexName& indexName) { builder->append(indexName); },
                            [&](const NaturalOrderHint& naturalOrderHint) {
                                builder->append(
                                    BSON(kNaturalFieldName << naturalOrderHint.direction));
                            }},
          _hint);
}

boost::optional<const IndexKeyPattern&> IndexHint::getIndexKeyPattern() const {
    if (!holds_alternative<IndexKeyPattern>(_hint)) {
        return {};
    }
    return get<IndexKeyPattern>(_hint);
}

boost::optional<const IndexName&> IndexHint::getIndexName() const {
    if (!holds_alternative<IndexName>(_hint)) {
        return {};
    }
    return get<IndexName>(_hint);
}

boost::optional<const NaturalOrderHint&> IndexHint::getNaturalHint() const {
    if (!holds_alternative<NaturalOrderHint>(_hint)) {
        return {};
    }
    return get<NaturalOrderHint>(_hint);
}

size_t IndexHint::hash() const {
    return visit(
        OverloadedVisitor{
            [&](const IndexKeyPattern& keyPattern) {
                return SimpleBSONObjComparator::kInstance.hash(keyPattern);
            },
            [&](const IndexName& indexName) { return boost::hash<std::string>{}(indexName); },
            [&](const NaturalOrderHint& naturalOrderHint) {
                return boost::hash<NaturalOrderHint::Direction>{}(naturalOrderHint.direction);
            }},
        _hint);
}

std::strong_ordering IndexHint::operator<=>(const IndexHint& other) const {
    if (auto cmp = _hint.valueless_by_exception() <=> other._hint.valueless_by_exception();
        !std::is_eq(cmp)) {
        return cmp;
    }

    if (auto cmp = _hint.index() <=> other._hint.index(); !std::is_eq(cmp)) {
        return cmp;
    }

    return std::visit(
        [&other](auto&& a) { return compare(a, std::get<std::decay_t<decltype(a)>>(other._hint)); },
        _hint);
};

bool IndexHint::operator==(const IndexHint& other) const {
    return std::is_eq(*this <=> other);
}

size_t hash_value(const IndexHint& hint) {
    return hint.hash();
}

};  // namespace mongo
