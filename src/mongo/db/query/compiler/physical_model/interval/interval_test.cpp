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

#include "mongo/db/query/compiler/physical_model/interval/interval.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

//
// Comparison
//

TEST(Comparison, Equality) {
    Interval a(BSON("" << 0 << "" << 10), true, true);
    ASSERT_EQUALS(a.compare(a), Interval::INTERVAL_EQUALS);

    Interval b(BSON("" << 0 << "" << 10), true, false);
    ASSERT_NOT_EQUALS(a.compare(b), Interval::INTERVAL_EQUALS);

    Interval c(BSON("" << 0 << "" << 10), false, true);
    ASSERT_NOT_EQUALS(a.compare(c), Interval::INTERVAL_EQUALS);

    Interval d(BSON("" << 0 << "" << 11), true, true);
    ASSERT_NOT_EQUALS(a.compare(d), Interval::INTERVAL_EQUALS);

    Interval e(BSON("" << 1 << "" << 10), true, true);
    ASSERT_NOT_EQUALS(a.compare(e), Interval::INTERVAL_EQUALS);
}

TEST(Comparison, Contains) {
    Interval a(BSON("" << 0 << "" << 10), true, true);
    Interval b(BSON("" << 1 << "" << 9), true, true);
    ASSERT_EQUALS(a.compare(b), Interval::INTERVAL_CONTAINS);

    Interval c(BSON("" << 0 << "" << 10), true, false);
    ASSERT_EQUALS(a.compare(c), Interval::INTERVAL_CONTAINS);

    Interval d(BSON("" << 0 << "" << 10), false, true);
    ASSERT_EQUALS(a.compare(d), Interval::INTERVAL_CONTAINS);

    Interval e(BSON("" << 0 << "" << 11), false, true);
    ASSERT_NOT_EQUALS(a.compare(e), Interval::INTERVAL_CONTAINS);
}

TEST(Comparison, Within) {
    Interval a(BSON("" << 0 << "" << 10), true, true);
    ASSERT_NOT_EQUALS(a.compare(a), Interval::INTERVAL_WITHIN);

    Interval b(BSON("" << 1 << "" << 9), true, true);
    ASSERT_EQUALS(b.compare(a), Interval::INTERVAL_WITHIN);

    Interval c(BSON("" << 0 << "" << 10), true, false);
    ASSERT_EQUALS(c.compare(a), Interval::INTERVAL_WITHIN);

    Interval d(BSON("" << 0 << "" << 10), false, true);
    ASSERT_EQUALS(d.compare(a), Interval::INTERVAL_WITHIN);

    Interval e(BSON("" << 0 << "" << 11), false, true);
    ASSERT_NOT_EQUALS(e.compare(a), Interval::INTERVAL_CONTAINS);
}

TEST(Comparison, OverlapsBefore) {
    Interval a(BSON("" << 1 << "" << 9), true, false);
    ASSERT_NOT_EQUALS(a.compare(a), Interval::INTERVAL_OVERLAPS_BEFORE);

    Interval b(BSON("" << 1 << "" << 9), false, true);
    ASSERT_EQUALS(a.compare(b), Interval::INTERVAL_OVERLAPS_BEFORE);

    Interval c(BSON("" << 1 << "" << 9), false, false);
    ASSERT_NOT_EQUALS(a.compare(c), Interval::INTERVAL_OVERLAPS_BEFORE);

    Interval d(BSON("" << 2 << "" << 10), true, true);
    ASSERT_EQUALS(a.compare(d), Interval::INTERVAL_OVERLAPS_BEFORE);

    Interval e(BSON("" << 0 << "" << 9), true, false);
    ASSERT_NOT_EQUALS(a.compare(e), Interval::INTERVAL_OVERLAPS_BEFORE);

    Interval f(BSON("" << 0 << "" << 8), true, false);
    ASSERT_NOT_EQUALS(a.compare(f), Interval::INTERVAL_OVERLAPS_BEFORE);
}

TEST(Comparison, OverlapsAfter) {
    Interval a(BSON("" << 1 << "" << 9), false, true);
    ASSERT_NOT_EQUALS(a.compare(a), Interval::INTERVAL_OVERLAPS_AFTER);

    Interval b(BSON("" << 1 << "" << 9), true, false);
    ASSERT_EQUALS(a.compare(b), Interval::INTERVAL_OVERLAPS_AFTER);

    Interval c(BSON("" << 1 << "" << 9), true, true);
    ASSERT_NOT_EQUALS(a.compare(c), Interval::INTERVAL_OVERLAPS_AFTER);

    Interval d(BSON("" << 2 << "" << 10), true, true);
    ASSERT_NOT_EQUALS(a.compare(d), Interval::INTERVAL_OVERLAPS_AFTER);

    Interval e(BSON("" << 0 << "" << 9), true, false);
    ASSERT_EQUALS(a.compare(e), Interval::INTERVAL_OVERLAPS_AFTER);
}

TEST(Comparison, Precedes) {
    Interval a(BSON("" << 10 << "" << 20), true, true);
    ASSERT_NOT_EQUALS(a.compare(a), Interval::INTERVAL_PRECEDES);

    Interval b(BSON("" << 0 << "" << 10), true, true);
    ASSERT_NOT_EQUALS(b.compare(a), Interval::INTERVAL_PRECEDES);

    Interval c(BSON("" << 0 << "" << 10), true, false);
    ASSERT_EQUALS(c.compare(a), Interval::INTERVAL_PRECEDES_COULD_UNION);

    Interval d(BSON("" << 0 << "" << 9), true, true);
    ASSERT_EQUALS(d.compare(a), Interval::INTERVAL_PRECEDES);

    Interval e(BSON("" << 5 << "" << 15), true, true);
    ASSERT_NOT_EQUALS(e.compare(a), Interval::INTERVAL_PRECEDES);

    Interval f(BSON("" << 5 << "" << 20), true, false);
    ASSERT_NOT_EQUALS(f.compare(a), Interval::INTERVAL_PRECEDES);
}

TEST(Comparison, PrecedesCouldUnion) {
    Interval a(BSON("" << 10 << "" << 20), false, true);
    ASSERT_NOT_EQUALS(a.compare(a), Interval::INTERVAL_PRECEDES);

    Interval b(BSON("" << 0 << "" << 10), true, false);
    ASSERT_EQUALS(b.compare(a), Interval::INTERVAL_PRECEDES);

    Interval c(BSON("" << 0 << "" << 10), true, true);
    ASSERT_EQUALS(c.compare(a), Interval::INTERVAL_PRECEDES_COULD_UNION);
}

TEST(Comparison, Succeds) {
    Interval a(BSON("" << 10 << "" << 20), true, true);
    ASSERT_NOT_EQUALS(a.compare(a), Interval::INTERVAL_SUCCEEDS);

    Interval b(BSON("" << 20 << "" << 30), true, true);
    ASSERT_NOT_EQUALS(b.compare(a), Interval::INTERVAL_SUCCEEDS);

    Interval c(BSON("" << 20 << "" << 30), false, true);
    ASSERT_EQUALS(c.compare(a), Interval::INTERVAL_SUCCEEDS);

    Interval d(BSON("" << 21 << "" << 30), true, true);
    ASSERT_EQUALS(d.compare(a), Interval::INTERVAL_SUCCEEDS);

    Interval e(BSON("" << 15 << "" << 30), true, true);
    ASSERT_NOT_EQUALS(e.compare(a), Interval::INTERVAL_SUCCEEDS);
}

//
// intersection
//

TEST(Intersection, Equals) {
    BSONObj itv = BSON("" << 10 << "" << 20);
    Interval a(itv, true, true);
    a.intersect(a);
    ASSERT_EQUALS(a.compare(Interval(itv, true, true)), Interval::INTERVAL_EQUALS);
}

TEST(Intersection, Contains) {
    Interval a(BSON("" << 10 << "" << 20), true, true);
    BSONObj itv = BSON("" << 11 << "" << 19);
    Interval b(itv, true, true);
    a.intersect(b);
    ASSERT_EQUALS(a.compare(b), Interval::INTERVAL_EQUALS);
}

TEST(Intersection, Within) {
    BSONObj itv = BSON("" << 10 << "" << 20);
    Interval a(itv, true, true);
    Interval b(BSON("" << 9 << "" << 21), true, true);
    a.intersect(b);
    ASSERT_EQUALS(a.compare(Interval(itv, true, true)), Interval::INTERVAL_EQUALS);
}

TEST(Intersection, OverlapsBefore) {
    Interval a(BSON("" << 10 << "" << 20), true, true);
    Interval b(BSON("" << 15 << "" << 25), true, true);
    a.intersect(b);

    BSONObj itv = BSON("" << 15 << "" << 20);
    ASSERT_EQUALS(a.compare(Interval(itv, true, true)), Interval::INTERVAL_EQUALS);
}

TEST(Intersection, OverlapsAfter) {
    Interval a(BSON("" << 10 << "" << 20), true, true);
    Interval b(BSON("" << 5 << "" << 15), true, true);
    a.intersect(b);

    BSONObj itv = BSON("" << 10 << "" << 15);
    ASSERT_EQUALS(a.compare(Interval(itv, true, true)), Interval::INTERVAL_EQUALS);
}

TEST(Intersection, Procedes) {
    Interval a(BSON("" << 10 << "" << 20), true, true);
    Interval b(BSON("" << 0 << "" << 5), true, true);
    a.intersect(b);

    ASSERT_TRUE(a.isEmpty());
}

TEST(Intersection, Succeds) {
    Interval a(BSON("" << 10 << "" << 20), true, true);
    Interval b(BSON("" << 25 << "" << 30), true, true);
    a.intersect(b);

    ASSERT_TRUE(a.isEmpty());
}

//
// combine (union)
//

TEST(Union, Equals) {
    BSONObj itv = BSON("" << 10 << "" << 20);
    Interval a(itv, true, true);
    a.combine(a);
    ASSERT_EQUALS(a.compare(Interval(itv, true, true)), Interval::INTERVAL_EQUALS);
}

TEST(Union, Contains) {
    BSONObj itv = BSON("" << 10 << "" << 20);
    Interval a(itv, true, true);
    Interval b(BSON("" << 11 << "" << 19), true, true);
    a.combine(b);
    ASSERT_EQUALS(a.compare(Interval(itv, true, true)), Interval::INTERVAL_EQUALS);
}

TEST(Union, Within) {
    Interval a(BSON("" << 10 << "" << 20), true, true);
    Interval b(BSON("" << 9 << "" << 21), true, true);
    a.combine(b);
    ASSERT_EQUALS(a.compare(b), Interval::INTERVAL_EQUALS);
}

TEST(Union, OverlapsBefore) {
    Interval a(BSON("" << 10 << "" << 20), true, true);
    Interval b(BSON("" << 15 << "" << 25), true, true);
    a.combine(b);
    BSONObj itv = BSON("" << 10 << "" << 25);
    ASSERT_EQUALS(a.compare(Interval(itv, true, true)), Interval::INTERVAL_EQUALS);
}

TEST(Union, OverlapsAfter) {
    Interval a(BSON("" << 10 << "" << 20), true, true);
    Interval b(BSON("" << 5 << "" << 15), true, true);
    a.combine(b);
    BSONObj itv = BSON("" << 5 << "" << 20);
    ASSERT_EQUALS(a.compare(Interval(itv, true, true)), Interval::INTERVAL_EQUALS);
}

TEST(Union, Precedes) {
    Interval a(BSON("" << 10 << "" << 20), true, true);
    Interval b(BSON("" << 20 << "" << 30), true, true);
    a.combine(b);
    BSONObj itv = BSON("" << 10 << "" << 30);
    ASSERT_EQUALS(a.compare(Interval(itv, true, true)), Interval::INTERVAL_EQUALS);
}

TEST(Union, Succeds) {
    Interval a(BSON("" << 10 << "" << 20), true, true);
    Interval b(BSON("" << 0 << "" << 5), true, true);
    a.combine(b);
    BSONObj itv = BSON("" << 0 << "" << 20);
    ASSERT_EQUALS(a.compare(Interval(itv, true, true)), Interval::INTERVAL_EQUALS);
}

TEST(Introspection, GetDirection) {
    // Empty/uninitialized Interval.
    boost::optional<Interval> i;
    i.emplace();
    ASSERT(i->getDirection() == Interval::Direction::kDirectionNone);

    // Empty Interval.
    i.emplace(BSON("" << 10 << "" << 10), false, false);
    ASSERT(i->getDirection() == Interval::Direction::kDirectionNone);

    // Point bound Interval.
    i.emplace(BSON("" << 10 << "" << 10), true, true);
    ASSERT(i->getDirection() == Interval::Direction::kDirectionNone);

    // Ascending interval.
    i.emplace(BSON("" << 10 << "" << 20), true, true);
    ASSERT(i->getDirection() == Interval::Direction::kDirectionAscending);

    // Descending interval.
    i.emplace(BSON("" << 11 << "" << 10), true, true);
    ASSERT(i->getDirection() == Interval::Direction::kDirectionDescending);
}

TEST(Introspection, MinToMax) {
    BSONObjBuilder builder;
    builder.appendMinKey("");
    builder.appendMaxKey("");
    BSONObj minMaxObj = builder.obj();

    Interval a(BSON("" << 10 << "" << 20), true, true);
    ASSERT_FALSE(a.isMinToMax());

    Interval b(minMaxObj, false, true);
    ASSERT_TRUE(b.isMinToMax());

    Interval c(minMaxObj, true, false);
    ASSERT_TRUE(c.isMinToMax());

    Interval d(minMaxObj, true, true);
    ASSERT_TRUE(d.isMinToMax());
}

TEST(Copying, ReverseClone) {
    Interval a(BSON("" << 10 << "" << 20), false, true);
    ASSERT(a.reverseClone() == Interval(BSON("" << 20 << "" << 10), true, false));
    ASSERT(a.reverseClone() != a);

    Interval b(BSON("" << 10 << "" << 5), true, true);
    ASSERT(b.reverseClone() == Interval(BSON("" << 5 << "" << 10), true, true));
    ASSERT(b.reverseClone() != b);

    Interval c(BSON("" << 1 << "" << 1), true, true);
    ASSERT(c.reverseClone() == c);
}


}  // unnamed namespace
}  // namespace mongo
