// fts_element_iterator_test.cpp
/**
*    Copyright (C) 2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/platform/basic.h"

#include "mongo/db/fts/fts_element_iterator.h"
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace fts {

using std::string;
using unittest::assertGet;

TEST(FTSElementIterator, Test1) {
    BSONObj obj = fromjson(
        "{ b : \"walking\","
        "  c : { e: \"walked\" },"
        "  d : \"walker\""
        "  }");

    BSONObj indexSpec = fromjson("{ key : { a : \"text\" }, weights : { b : 10, d : 5 } }");

    FTSSpec spec(assertGet(FTSSpec::fixSpec(indexSpec)));
    Weights::const_iterator itt = spec.weights().begin();
    ASSERT(itt != spec.weights().end());
    ASSERT_EQUALS("a", itt->first);
    ASSERT_EQUALS(1, itt->second);
    ++itt;
    ASSERT(itt != spec.weights().end());
    ASSERT_EQUALS("b", itt->first);
    ASSERT_EQUALS(10, itt->second);
    ++itt;
    ASSERT(itt != spec.weights().end());
    ASSERT_EQUALS("d", itt->first);
    ASSERT_EQUALS(5, itt->second);
    ++itt;

    FTSElementIterator it(spec, obj);

    ASSERT(it.more());
    FTSIteratorValue val = it.next();
    ASSERT_EQUALS("walking", string(val._text));
    ASSERT_EQUALS("english", val._language->str());
    ASSERT_EQUALS(10, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("walker", string(val._text));
    ASSERT_EQUALS("english", val._language->str());
    ASSERT_EQUALS(5, val._weight);
}

// Multi-language : test
TEST(FTSElementIterator, Test2) {
    BSONObj obj = fromjson(
        "{ a :"
        "  { b :"
        "    [ { c : \"walked\", language : \"english\" },"
        "      { c : \"camminato\", language : \"italian\" },"
        "      { c : \"ging\", language : \"german\" } ]"
        "   },"
        "  d : \"Feliz Año Nuevo!\","
        "  language : \"spanish\""
        " }");

    BSONObj indexSpec = fromjson("{ key : { \"a.b.c\" : \"text\", d : \"text\" } }");

    FTSSpec spec(assertGet(FTSSpec::fixSpec(indexSpec)));

    FTSElementIterator it(spec, obj);

    ASSERT(it.more());
    FTSIteratorValue val = it.next();
    ASSERT_EQUALS("walked", string(val._text));
    ASSERT_EQUALS("english", val._language->str());
    ASSERT_EQUALS(1, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("camminato", string(val._text));
    ASSERT_EQUALS("italian", val._language->str());
    ASSERT_EQUALS(1, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("ging", string(val._text));
    ASSERT_EQUALS("german", val._language->str());
    ASSERT_EQUALS(1, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("Feliz Año Nuevo!", string(val._text));
    ASSERT_EQUALS("spanish", val._language->str());
    ASSERT_EQUALS(1, val._weight);
}

// Multi-language : test nested stemming per sub-document
TEST(FTSElementIterator, Test3) {
    BSONObj obj = fromjson(
        "{ language : \"english\","
        "  a :"
        "  { language : \"danish\","
        "    b :"
        "    [ { c : \"foredrag\" },"
        "      { c : \"foredragsholder\" },"
        "      { c : \"lector\" } ]"
        "  }"
        "}");

    BSONObj indexSpec =
        fromjson("{ key : { a : \"text\", \"a.b.c\" : \"text\" }, weights : { \"a.b.c\" : 5 } }");

    FTSSpec spec(assertGet(FTSSpec::fixSpec(indexSpec)));
    Weights::const_iterator itt = spec.weights().begin();
    ASSERT(itt != spec.weights().end());
    ASSERT_EQUALS("a", itt->first);
    ASSERT_EQUALS(1, itt->second);
    ++itt;
    ASSERT(itt != spec.weights().end());
    ASSERT_EQUALS("a.b.c", itt->first);
    ASSERT_EQUALS(5, itt->second);

    FTSElementIterator it(spec, obj);

    ASSERT(it.more());
    FTSIteratorValue val = it.next();
    ASSERT_EQUALS("foredrag", string(val._text));
    ASSERT_EQUALS("danish", val._language->str());
    ASSERT_EQUALS(5, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("foredragsholder", string(val._text));
    ASSERT_EQUALS("danish", val._language->str());
    ASSERT_EQUALS(5, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("lector", string(val._text));
    ASSERT_EQUALS("danish", val._language->str());
    ASSERT_EQUALS(5, val._weight);
}

// Multi-language : test nested arrays
TEST(FTSElementIterator, Test4) {
    BSONObj obj = fromjson(
        "{ language : \"english\","
        "  a : ["
        "  { language : \"danish\","
        "    b :"
        "    [ { c : [\"foredrag\"] },"
        "      { c : [\"foredragsholder\"] },"
        "      { c : [\"lector\"] } ]"
        "  } ]"
        "}");

    BSONObj indexSpec = fromjson("{ key : { \"a.b.c\" : \"text\" }, weights : { \"a.b.c\" : 5 } }");

    FTSSpec spec(assertGet(FTSSpec::fixSpec(indexSpec)));
    FTSElementIterator it(spec, obj);

    ASSERT(it.more());
    FTSIteratorValue val = it.next();
    ASSERT_EQUALS("foredrag", string(val._text));
    ASSERT_EQUALS("danish", val._language->str());
    ASSERT_EQUALS(5, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("foredragsholder", string(val._text));
    ASSERT_EQUALS("danish", val._language->str());
    ASSERT_EQUALS(5, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("lector", string(val._text));
    ASSERT_EQUALS("danish", val._language->str());
    ASSERT_EQUALS(5, val._weight);
}

// Multi-language : test wildcard spec
TEST(FTSElementIterator, Test5) {
    BSONObj obj = fromjson(
        "{ language : \"english\","
        "  b : \"these boots were made for walking\","
        "  c : { e: \"I walked half way to the market before seeing the sunrise\" },"
        "  d : "
        "  { language : \"danish\","
        "    e :"
        "    [ { f : \"foredrag\", g : 12 },"
        "      { f : \"foredragsholder\", g : 13 },"
        "      { f : \"lector\", g : 14 } ]"
        "  }"
        "}");

    BSONObj indexSpec =
        fromjson("{ key : { a : \"text\" }, weights : { b : 20, c : 10, \"d.e.f\" : 5 } }");

    FTSSpec spec(assertGet(FTSSpec::fixSpec(indexSpec)));
    FTSElementIterator it(spec, obj);

    ASSERT(it.more());
    FTSIteratorValue val = it.next();
    ASSERT_EQUALS("these boots were made for walking", string(val._text));
    ASSERT_EQUALS("english", val._language->str());
    ASSERT_EQUALS(20, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("foredrag", string(val._text));
    ASSERT_EQUALS("danish", val._language->str());
    ASSERT_EQUALS(5, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("foredragsholder", string(val._text));
    ASSERT_EQUALS("danish", val._language->str());
    ASSERT_EQUALS(5, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("lector", string(val._text));
    ASSERT_EQUALS("danish", val._language->str());
    ASSERT_EQUALS(5, val._weight);
}

// Multi-language : test wildcard spec
TEST(FTSElementIterator, Test6) {
    BSONObj obj = fromjson(
        "{ language : \"english\","
        "  b : \"these boots were made for walking\","
        "  c : { e: \"I walked half way to the market before seeing the sunrise\" },"
        "  d : "
        "  { language : \"danish\","
        "    e :"
        "    [ { f : \"foredrag\", g : 12 },"
        "      { f : \"foredragsholder\", g : 13 },"
        "      { f : \"lector\", g : 14 } ]"
        "  }"
        "}");

    BSONObj indexSpec =
        fromjson("{ key : { a : \"text\" }, weights : { b : 20, c : 10, \"d.e.f\" : 5 } }");

    FTSSpec spec(assertGet(FTSSpec::fixSpec(indexSpec)));
    FTSElementIterator it(spec, obj);

    ASSERT(it.more());
    FTSIteratorValue val = it.next();
    ASSERT_EQUALS("these boots were made for walking", string(val._text));
    ASSERT_EQUALS("english", val._language->str());
    ASSERT_EQUALS(20, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("foredrag", string(val._text));
    ASSERT_EQUALS("danish", val._language->str());
    ASSERT_EQUALS(5, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("foredragsholder", string(val._text));
    ASSERT_EQUALS("danish", val._language->str());
    ASSERT_EQUALS(5, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("lector", string(val._text));
    ASSERT_EQUALS("danish", val._language->str());
    ASSERT_EQUALS(5, val._weight);
}

// Multi-language : Test Version 2 Language Override
TEST(FTSElementIterator, LanguageOverrideV2) {
    BSONObj obj = fromjson(
        "{ a :"
        "  { b :"
        "    [ { c : \"walked\", language : \"english\" },"
        "      { c : \"camminato\", language : \"italian\" },"
        "      { c : \"ging\", language : \"german\" } ]"
        "   },"
        "  d : \"Feliz Año Nuevo!\","
        "  language : \"spanish\""
        " }");

    BSONObj indexSpec =
        fromjson("{ key : { \"a.b.c\" : \"text\", d : \"text\" }, textIndexVersion: 2.0 }");

    FTSSpec spec(assertGet(FTSSpec::fixSpec(indexSpec)));

    FTSElementIterator it(spec, obj);

    ASSERT(it.more());
    FTSIteratorValue val = it.next();
    ASSERT_EQUALS("walked", string(val._text));
    ASSERT_EQUALS("english", val._language->str());
    ASSERT_EQUALS(val._language, FTSLanguage::make(val._language->str(), TEXT_INDEX_VERSION_2));
    ASSERT_EQUALS(1, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("camminato", string(val._text));
    ASSERT_EQUALS("italian", val._language->str());
    ASSERT_EQUALS(val._language, FTSLanguage::make(val._language->str(), TEXT_INDEX_VERSION_2));
    ASSERT_EQUALS(1, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("ging", string(val._text));
    ASSERT_EQUALS("german", val._language->str());
    ASSERT_EQUALS(val._language, FTSLanguage::make(val._language->str(), TEXT_INDEX_VERSION_2));
    ASSERT_EQUALS(1, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("Feliz Año Nuevo!", string(val._text));
    ASSERT_EQUALS("spanish", val._language->str());
    ASSERT_EQUALS(val._language, FTSLanguage::make(val._language->str(), TEXT_INDEX_VERSION_2));
    ASSERT_EQUALS(1, val._weight);
}

// Multi-language : Test Version 3 Language Override
TEST(FTSElementIterator, LanguageOverrideV3) {
    BSONObj obj = fromjson(
        "{ a :"
        "  { b :"
        "    [ { c : \"walked\", language : \"english\" },"
        "      { c : \"camminato\", language : \"italian\" },"
        "      { c : \"ging\", language : \"german\" } ]"
        "   },"
        "  d : \"Feliz Año Nuevo!\","
        "  language : \"spanish\""
        " }");

    BSONObj indexSpec =
        fromjson("{ key : { \"a.b.c\" : \"text\", d : \"text\" }, textIndexVersion: 3.0 }");

    FTSSpec spec(assertGet(FTSSpec::fixSpec(indexSpec)));

    FTSElementIterator it(spec, obj);

    ASSERT(it.more());
    FTSIteratorValue val = it.next();
    ASSERT_EQUALS("walked", string(val._text));
    ASSERT_EQUALS("english", val._language->str());
    ASSERT_EQUALS(val._language, FTSLanguage::make(val._language->str(), TEXT_INDEX_VERSION_3));
    ASSERT_EQUALS(1, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("camminato", string(val._text));
    ASSERT_EQUALS("italian", val._language->str());
    ASSERT_EQUALS(val._language, FTSLanguage::make(val._language->str(), TEXT_INDEX_VERSION_3));
    ASSERT_EQUALS(1, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("ging", string(val._text));
    ASSERT_EQUALS("german", val._language->str());
    ASSERT_EQUALS(val._language, FTSLanguage::make(val._language->str(), TEXT_INDEX_VERSION_3));
    ASSERT_EQUALS(1, val._weight);

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("Feliz Año Nuevo!", string(val._text));
    ASSERT_EQUALS("spanish", val._language->str());
    ASSERT_EQUALS(val._language, FTSLanguage::make(val._language->str(), TEXT_INDEX_VERSION_3));
    ASSERT_EQUALS(1, val._weight);
}

}  // namespace fts
}  // namespace mongo
