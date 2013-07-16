/**
 *    Copyright 2013 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/db/ops/log_builder.h"

#include "mongo/base/status.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

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

    TEST(LogBuilder, AddOneToUnset) {
        mmb::Document doc;
        LogBuilder lb(doc.root());

        const mmb::Element elt_xy = doc.makeElementInt("x.y", 1);
        ASSERT_TRUE(elt_xy.ok());
        ASSERT_OK(lb.addToUnsets(elt_xy));

        ASSERT_EQUALS(mongo::fromjson("{ $unset : { 'x.y' : 1 } }"), doc);
    }

    TEST(LogBuilder, AddOneToEach) {
        mmb::Document doc;
        LogBuilder lb(doc.root());

        const mmb::Element elt_ab = doc.makeElementInt("a.b", 1);
        ASSERT_TRUE(elt_ab.ok());
        ASSERT_OK(lb.addToSets(elt_ab));

        const mmb::Element elt_xy = doc.makeElementInt("x.y", 1);
        ASSERT_TRUE(elt_xy.ok());
        ASSERT_OK(lb.addToUnsets(elt_xy));

        ASSERT_EQUALS(
            mongo::fromjson(
                "{ "
                "   $set : { 'a.b' : 1 }, "
                "   $unset : { 'x.y' : 1 } "
                "}"
                ), doc);
    }

    TEST(LogBuilder, AddOneObjectReplacementEntry) {
        mmb::Document doc;
        LogBuilder lb(doc.root());

        mmb::Element replacement = doc.end();
        ASSERT_FALSE(replacement.ok());
        ASSERT_OK(lb.getReplacementObject(&replacement));
        ASSERT_TRUE(replacement.ok());
        ASSERT_TRUE(replacement.isType(mongo::Object));

        const mmb::Element elt_a = doc.makeElementInt("a", 1);
        ASSERT_TRUE(elt_a.ok());
        ASSERT_OK(replacement.pushBack(elt_a));

        ASSERT_EQUALS(mongo::fromjson("{ a : 1 }"), doc);
    }

    TEST(LogBuilder, AddTwoObjectReplacementEntry) {
        mmb::Document doc;
        LogBuilder lb(doc.root());

        mmb::Element replacement = doc.end();
        ASSERT_FALSE(replacement.ok());
        ASSERT_OK(lb.getReplacementObject(&replacement));
        ASSERT_TRUE(replacement.ok());
        ASSERT_TRUE(replacement.isType(mongo::Object));

        const mmb::Element elt_a = doc.makeElementInt("a", 1);
        ASSERT_TRUE(elt_a.ok());
        ASSERT_OK(replacement.pushBack(elt_a));

        const mmb::Element elt_b = doc.makeElementInt("b", 2);
        ASSERT_TRUE(elt_b.ok());
        ASSERT_OK(replacement.pushBack(elt_b));

        ASSERT_EQUALS(mongo::fromjson("{ a : 1, b: 2 }"), doc);
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

        ASSERT_EQUALS(
            mongo::fromjson(
                "{ $set : {"
                "   'a.b' : 1, "
                "   'x.y' : 1 "
                "} }"
                ), doc);
    }

    TEST(LogBuilder, VerifyUnsetsAreGrouped) {
        mmb::Document doc;
        LogBuilder lb(doc.root());

        const mmb::Element elt_ab = doc.makeElementInt("a.b", 1);
        ASSERT_TRUE(elt_ab.ok());
        ASSERT_OK(lb.addToUnsets(elt_ab));

        const mmb::Element elt_xy = doc.makeElementInt("x.y", 1);
        ASSERT_TRUE(elt_xy.ok());
        ASSERT_OK(lb.addToUnsets(elt_xy));

        ASSERT_EQUALS(
            mongo::fromjson(
                "{ $unset : {"
                "   'a.b' : 1, "
                "   'x.y' : 1 "
                "} }"
                ), doc);
    }

    TEST(LogBuilder, PresenceOfSetPreventsObjectReplacement) {
        mmb::Document doc;
        LogBuilder lb(doc.root());

        mmb::Element replacement = doc.end();
        ASSERT_FALSE(replacement.ok());
        ASSERT_OK(lb.getReplacementObject(&replacement));
        ASSERT_TRUE(replacement.ok());

        const mmb::Element elt_ab = doc.makeElementInt("a.b", 1);
        ASSERT_TRUE(elt_ab.ok());
        ASSERT_OK(lb.addToSets(elt_ab));

        replacement = doc.end();
        ASSERT_FALSE(replacement.ok());
        ASSERT_NOT_OK(lb.getReplacementObject(&replacement));
        ASSERT_FALSE(replacement.ok());
    }

    TEST(LogBuilder, PresenceOfUnsetPreventsObjectReplacement) {
        mmb::Document doc;
        LogBuilder lb(doc.root());

        mmb::Element replacement = doc.end();
        ASSERT_FALSE(replacement.ok());
        ASSERT_OK(lb.getReplacementObject(&replacement));
        ASSERT_TRUE(replacement.ok());

        const mmb::Element elt_ab = doc.makeElementInt("a.b", 1);
        ASSERT_TRUE(elt_ab.ok());
        ASSERT_OK(lb.addToSets(elt_ab));

        replacement = doc.end();
        ASSERT_FALSE(replacement.ok());
        ASSERT_NOT_OK(lb.getReplacementObject(&replacement));
        ASSERT_FALSE(replacement.ok());
    }

    TEST(LogBuilder, CantAddSetWithObjectReplacementDataPresent) {
        mmb::Document doc;
        LogBuilder lb(doc.root());

        mmb::Element replacement = doc.end();
        ASSERT_FALSE(replacement.ok());
        ASSERT_OK(lb.getReplacementObject(&replacement));
        ASSERT_TRUE(replacement.ok());
        ASSERT_OK(replacement.appendInt("a", 1));

        mmb::Element setCandidate = doc.makeElementInt("x", 0);
        ASSERT_NOT_OK(lb.addToSets(setCandidate));
    }

    TEST(LogBuilder, CantAddUnsetWithObjectReplacementDataPresent) {
        mmb::Document doc;
        LogBuilder lb(doc.root());

        mmb::Element replacement = doc.end();
        ASSERT_FALSE(replacement.ok());
        ASSERT_OK(lb.getReplacementObject(&replacement));
        ASSERT_TRUE(replacement.ok());
        ASSERT_OK(replacement.appendInt("a", 1));

        mmb::Element setCandidate = doc.makeElementInt("x", 0);
        ASSERT_NOT_OK(lb.addToUnsets(setCandidate));
    }

    // Ensure that once you have obtained the object replacement slot and mutated it, that the
    // object replacement slot becomes in accessible. This is a bit paranoid, since in practice
    // the modifier conflict detection logic should prevent that outcome at a higher level, but
    // preventing it here costs us nothing and add an extra safety check.
    TEST(LogBuilder, CantReacquireObjectReplacementData) {
        mmb::Document doc;
        LogBuilder lb(doc.root());

        mmb::Element replacement = doc.end();
        ASSERT_FALSE(replacement.ok());
        ASSERT_OK(lb.getReplacementObject(&replacement));
        ASSERT_TRUE(replacement.ok());
        ASSERT_OK(replacement.appendInt("a", 1));

        mmb::Element again = doc.end();
        ASSERT_FALSE(again.ok());
        ASSERT_NOT_OK(lb.getReplacementObject(&again));
        ASSERT_FALSE(again.ok());
    }

} // namespace
