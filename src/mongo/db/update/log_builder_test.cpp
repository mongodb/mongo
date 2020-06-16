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

#include "mongo/db/update/log_builder.h"

#include "mongo/base/status.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/safe_num.h"

namespace {

namespace mmb = mongo::mutablebson;
using mongo::LogBuilder;

TEST(LogBuilder, Initialization) {
    mmb::Document doc;
    LogBuilder lb(doc.root());
    ASSERT_EQUALS(&doc, &lb.getDocument());
}

TEST(LogBuilder, AddOneToSet) {
    mmb::Document doc;
    LogBuilder lb(doc.root());

    const mmb::Element elt_ab = doc.makeElementInt("a.b", 1);
    ASSERT_TRUE(elt_ab.ok());
    ASSERT_OK(lb.addToSets(elt_ab));

    ASSERT_EQUALS(mongo::fromjson("{ $set : { 'a.b' : 1 } }"), doc);
}

TEST(LogBuilder, AddElementToSet) {
    mmb::Document doc;
    LogBuilder lb(doc.root());

    const mmb::Element elt_ab = doc.makeElementInt("", 1);
    ASSERT_TRUE(elt_ab.ok());
    ASSERT_OK(lb.addToSetsWithNewFieldName("a.b", elt_ab));

    ASSERT_EQUALS(mongo::fromjson("{ $set : { 'a.b' : 1 } }"), doc);
}

TEST(LogBuilder, AddBSONElementToSet) {
    mmb::Document doc;
    LogBuilder lb(doc.root());

    mongo::BSONObj obj = mongo::fromjson("{'':1}");

    ASSERT_OK(lb.addToSetsWithNewFieldName("a.b", obj.firstElement()));

    ASSERT_EQUALS(mongo::fromjson("{ $set : { 'a.b' : 1 } }"), doc);
}

TEST(LogBuilder, AddSafeNumToSet) {
    mmb::Document doc;
    LogBuilder lb(doc.root());

    mongo::BSONObj obj = mongo::fromjson("{'':1}");

    ASSERT_OK(lb.addToSets("a.b", mongo::SafeNum(1)));

    ASSERT_EQUALS(mongo::fromjson("{ $set : { 'a.b' : 1 } }"), doc);
}

TEST(LogBuilder, AddOneToUnset) {
    mmb::Document doc;
    LogBuilder lb(doc.root());
    ASSERT_OK(lb.addToUnsets("x.y"));
    ASSERT_EQUALS(mongo::fromjson("{ $unset : { 'x.y' : true } }"), doc);
}

TEST(LogBuilder, AddOneToEach) {
    mmb::Document doc;
    LogBuilder lb(doc.root());

    const mmb::Element elt_ab = doc.makeElementInt("a.b", 1);
    ASSERT_TRUE(elt_ab.ok());
    ASSERT_OK(lb.addToSets(elt_ab));

    ASSERT_OK(lb.addToUnsets("x.y"));

    ASSERT_EQUALS(mongo::fromjson("{ "
                                  "   $set : { 'a.b' : 1 }, "
                                  "   $unset : { 'x.y' : true } "
                                  "}"),
                  doc);
}

TEST(LogBuilder, VerifySetsAreGrouped) {
    mmb::Document doc;
    LogBuilder lb(doc.root());

    const mmb::Element elt_ab = doc.makeElementInt("a.b", 1);
    ASSERT_TRUE(elt_ab.ok());
    ASSERT_OK(lb.addToSets(elt_ab));

    const mmb::Element elt_xy = doc.makeElementInt("x.y", 1);
    ASSERT_TRUE(elt_xy.ok());
    ASSERT_OK(lb.addToSets(elt_xy));

    ASSERT_EQUALS(mongo::fromjson("{ $set : {"
                                  "   'a.b' : 1, "
                                  "   'x.y' : 1 "
                                  "} }"),
                  doc);
}

TEST(LogBuilder, VerifyUnsetsAreGrouped) {
    mmb::Document doc;
    LogBuilder lb(doc.root());

    ASSERT_OK(lb.addToUnsets("a.b"));
    ASSERT_OK(lb.addToUnsets("x.y"));

    ASSERT_EQUALS(mongo::fromjson("{ $unset : {"
                                  "   'a.b' : true, "
                                  "   'x.y' : true "
                                  "} }"),
                  doc);
}
}  // namespace
