// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/fts_element_iterator.h"

#include "mongo/base/status.h"
#include "mongo/bson/json.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/unittest/unittest.h"

#include <map>
#include <memory>
#include <utility>

#include <fmt/format.h>

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
    ASSERT_EQUALS("walking", string(val.text()));
    ASSERT_EQUALS("english", val.language()->str());
    ASSERT_EQUALS(10, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("walker", string(val.text()));
    ASSERT_EQUALS("english", val.language()->str());
    ASSERT_EQUALS(5, val.weight());
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
    ASSERT_EQUALS("walked", string(val.text()));
    ASSERT_EQUALS("english", val.language()->str());
    ASSERT_EQUALS(1, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("camminato", string(val.text()));
    ASSERT_EQUALS("italian", val.language()->str());
    ASSERT_EQUALS(1, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("ging", string(val.text()));
    ASSERT_EQUALS("german", val.language()->str());
    ASSERT_EQUALS(1, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("Feliz Año Nuevo!", string(val.text()));
    ASSERT_EQUALS("spanish", val.language()->str());
    ASSERT_EQUALS(1, val.weight());
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
    ASSERT_EQUALS("foredrag", string(val.text()));
    ASSERT_EQUALS("danish", val.language()->str());
    ASSERT_EQUALS(5, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("foredragsholder", string(val.text()));
    ASSERT_EQUALS("danish", val.language()->str());
    ASSERT_EQUALS(5, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("lector", string(val.text()));
    ASSERT_EQUALS("danish", val.language()->str());
    ASSERT_EQUALS(5, val.weight());
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
    ASSERT_EQUALS("foredrag", string(val.text()));
    ASSERT_EQUALS("danish", val.language()->str());
    ASSERT_EQUALS(5, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("foredragsholder", string(val.text()));
    ASSERT_EQUALS("danish", val.language()->str());
    ASSERT_EQUALS(5, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("lector", string(val.text()));
    ASSERT_EQUALS("danish", val.language()->str());
    ASSERT_EQUALS(5, val.weight());
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
    ASSERT_EQUALS("these boots were made for walking", string(val.text()));
    ASSERT_EQUALS("english", val.language()->str());
    ASSERT_EQUALS(20, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("foredrag", string(val.text()));
    ASSERT_EQUALS("danish", val.language()->str());
    ASSERT_EQUALS(5, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("foredragsholder", string(val.text()));
    ASSERT_EQUALS("danish", val.language()->str());
    ASSERT_EQUALS(5, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("lector", string(val.text()));
    ASSERT_EQUALS("danish", val.language()->str());
    ASSERT_EQUALS(5, val.weight());
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
    ASSERT_EQUALS("these boots were made for walking", string(val.text()));
    ASSERT_EQUALS("english", val.language()->str());
    ASSERT_EQUALS(20, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("foredrag", string(val.text()));
    ASSERT_EQUALS("danish", val.language()->str());
    ASSERT_EQUALS(5, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("foredragsholder", string(val.text()));
    ASSERT_EQUALS("danish", val.language()->str());
    ASSERT_EQUALS(5, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("lector", string(val.text()));
    ASSERT_EQUALS("danish", val.language()->str());
    ASSERT_EQUALS(5, val.weight());
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
    ASSERT_EQUALS("walked", string(val.text()));
    ASSERT_EQUALS("english", val.language()->str());
    ASSERT_EQUALS(val.language(), &FTSLanguage::make(val.language()->str(), TEXT_INDEX_VERSION_2));
    ASSERT_EQUALS(1, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("camminato", string(val.text()));
    ASSERT_EQUALS("italian", val.language()->str());
    ASSERT_EQUALS(val.language(), &FTSLanguage::make(val.language()->str(), TEXT_INDEX_VERSION_2));
    ASSERT_EQUALS(1, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("ging", string(val.text()));
    ASSERT_EQUALS("german", val.language()->str());
    ASSERT_EQUALS(val.language(), &FTSLanguage::make(val.language()->str(), TEXT_INDEX_VERSION_2));
    ASSERT_EQUALS(1, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("Feliz Año Nuevo!", string(val.text()));
    ASSERT_EQUALS("spanish", val.language()->str());
    ASSERT_EQUALS(val.language(), &FTSLanguage::make(val.language()->str(), TEXT_INDEX_VERSION_2));
    ASSERT_EQUALS(1, val.weight());
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
    ASSERT_EQUALS("walked", string(val.text()));
    ASSERT_EQUALS("english", val.language()->str());
    ASSERT_EQUALS(val.language(), &FTSLanguage::make(val.language()->str(), TEXT_INDEX_VERSION_3));
    ASSERT_EQUALS(1, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("camminato", string(val.text()));
    ASSERT_EQUALS("italian", val.language()->str());
    ASSERT_EQUALS(val.language(), &FTSLanguage::make(val.language()->str(), TEXT_INDEX_VERSION_3));
    ASSERT_EQUALS(1, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("ging", string(val.text()));
    ASSERT_EQUALS("german", val.language()->str());
    ASSERT_EQUALS(val.language(), &FTSLanguage::make(val.language()->str(), TEXT_INDEX_VERSION_3));
    ASSERT_EQUALS(1, val.weight());

    ASSERT(it.more());
    val = it.next();
    ASSERT_EQUALS("Feliz Año Nuevo!", string(val.text()));
    ASSERT_EQUALS("spanish", val.language()->str());
    ASSERT_EQUALS(val.language(), &FTSLanguage::make(val.language()->str(), TEXT_INDEX_VERSION_3));
    ASSERT_EQUALS(1, val.weight());
}

}  // namespace fts
}  // namespace mongo
