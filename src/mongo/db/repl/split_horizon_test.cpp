/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/split_horizon.h"

#include <algorithm>
#include <boost/optional.hpp>
#include <iterator>

#include "mongo/stdx/utility.h"
#include "mongo/unittest/unittest.h"

using namespace std::literals::string_literals;

namespace mongo {
namespace repl {
namespace {
static const std::string defaultHost = "default.dns.name.example.com";
static const std::string defaultPort = "4242";
static const std::string defaultHostAndPort = defaultHost + ":" + defaultPort;

static const std::string matchingHost = "matching.dns.name.example.com";
static const std::string matchingPort = "4243";
static const std::string matchingHostAndPort = matchingHost + ":" + matchingPort;


static const std::string nonmatchingHost = "nonmatching.dns.name.example.com";
static const std::string nonmatchingPort = "4244";
static const std::string nonmatchingHostAndPort = nonmatchingHost + ":" + nonmatchingPort;

static const std::string altPort = ":666";

using MappingType = std::map<std::string, std::string>;

SplitHorizon::ForwardMapping populateForwardMap(const MappingType& mapping) {
    SplitHorizon::ForwardMapping forwardMapping;
    forwardMapping.emplace(SplitHorizon::kDefaultHorizon, defaultHostAndPort);

    using ForwardMappingValueType = decltype(forwardMapping)::value_type;
    using ElementType = MappingType::value_type;
    auto createForwardMapping = [](const ElementType& element) {
        return ForwardMappingValueType{element.first, HostAndPort(element.second)};
    };
    std::transform(begin(mapping),
                   end(mapping),
                   inserter(forwardMapping, end(forwardMapping)),
                   createForwardMapping);

    return forwardMapping;
}

TEST(SplitHorizonTesting, determineHorizon) {
    struct Input {
        SplitHorizon::ForwardMapping forwardMapping;  // Will get "__default" added to it.
        SplitHorizon::Parameters horizonParameters;

        Input(const MappingType& mapping, boost::optional<std::string> sniName)
            : forwardMapping(populateForwardMap(mapping)), horizonParameters(std::move(sniName)) {}
    };

    struct {
        Input input;

        std::string expected;
    } tests[] = {
        // No parameters and no horizon views configured.
        {{{}, boost::none}, "__default"},
        {{{}, defaultHost}, "__default"},

        // No SNI -> no match
        {{{{"unusedHorizon", "badmatch:00001"}}, boost::none}, "__default"},

        // Unmatching SNI -> no match
        {{{{"unusedHorizon", "badmatch:00001"}}, nonmatchingHost}, "__default"},

        // Matching SNI -> match
        {{{{"targetHorizon", matchingHostAndPort}}, matchingHost}, "targetHorizon"},
    };

    for (const auto& test : tests) {
        const auto& expected = test.expected;
        const auto& input = test.input;

        const std::string witness =
            SplitHorizon(input.forwardMapping).determineHorizon(input.horizonParameters).toString();
        ASSERT_EQUALS(witness, expected);
    }

    const Input failingCtorCases[] = {
        // Matching SNI, different port, collision -> fails
        {{{"targetHorizon", matchingHost + altPort}, {"badHorizon", matchingHostAndPort}},
         matchingHost},

        // Default horizon ambiguous case is a failure
        {{{"targetHorizon", defaultHost + altPort}, {"badHorizon", nonmatchingHostAndPort}},
         defaultHost},
    };

    for (const auto& input : failingCtorCases) {
        ASSERT_THROWS(SplitHorizon(input.forwardMapping), ExceptionFor<ErrorCodes::BadValue>);
    }
}

TEST(SplitHorizonTesting, basicConstruction) {
    struct Input {
        SplitHorizon::ForwardMapping forwardMapping;  // Will get "__default" added to it.

        Input(const MappingType& mapping) : forwardMapping(populateForwardMap(mapping)) {}
    };

    const struct {
        Input input;
        ErrorCodes::Error expectedErrorCode;
        std::vector<std::string> expectedErrorMessageFragments;
        std::vector<std::string> absentErrorMessageFragments;
    } tests[] = {
        // Empty case (the `Input` type constructs the expected "__default" member.)
        {{{}}, ErrorCodes::OK, {}, {}},

        // A single horizon case, with no conflicts.
        {{{{"extraHorizon", "example.com:42"}}}, ErrorCodes::OK, {}, {}},

        // Two horizons with no conflicts
        {{{{"extraHorizon", "example.com:42"}, {"extraHorizon2", "extra.example.com:42"}}},
         ErrorCodes::OK,
         {},
         {}},

        // Three horizons with no conflicts
        {{{{"extraHorizon", "example.com:42"},
           {"extraHorizon2", "extra.example.com:42"},
           {"extraHorizon3", "extra3.example.com" + altPort}}},
         ErrorCodes::OK,
         {},
         {}},

        // Two horizons, with the same host and port
        {{{{"horizon1", "same.example.com:42"}, {"horizon2", "same.example.com:42"}}},
         ErrorCodes::BadValue,
         {"Duplicate horizon member found", "same.example.com"},
         {}},

        // Two horizons, with the same host and different port
        {{{{"horizon1", "same.example.com:42"}, {"horizon2", "same.example.com:43"}}},
         ErrorCodes::BadValue,
         {"Duplicate horizon member found", "same.example.com"},
         {}},

        // Three horizons, with two of them having the same host and port (checking that
        // the distinct horizon isn't reported in the error message.
        {{{{"horizon1", "same.example.com:42"},
           {"horizon2", "different.example.com:42"},
           {"horizon3", "same.example.com:42"}}},
         ErrorCodes::BadValue,
         {"Duplicate horizon member found", "same.example.com"},
         {"different.example.com"}},
    };

    for (const auto& test : tests) {
        const auto& input = test.input;
        const auto& expectedErrorCode = test.expectedErrorCode;
        const auto horizonOpt = [&]() -> boost::optional<SplitHorizon> {
            try {
                return SplitHorizon(input.forwardMapping);
            } catch (const DBException& ex) {
                ASSERT_NOT_EQUALS(expectedErrorCode, ErrorCodes::OK);
                ASSERT_EQUALS(ex.toStatus().code(), expectedErrorCode);
                for (const auto& fragment : test.expectedErrorMessageFragments) {
                    ASSERT_NOT_EQUALS(ex.toStatus().reason().find(fragment), std::string::npos);
                }
                for (const auto& fragment : test.absentErrorMessageFragments) {
                    ASSERT_EQUALS(ex.toStatus().reason().find(fragment), std::string::npos);
                }
                return boost::none;
            }
        }();

        if (!horizonOpt)
            continue;
        ASSERT_EQUALS(expectedErrorCode, ErrorCodes::OK);

        const auto& horizon = *horizonOpt;

        for (const auto& element : input.forwardMapping) {
            {
                const auto found = horizon.getForwardMappings().find(element.first);
                ASSERT_TRUE(found != end(horizon.getForwardMappings()));
                ASSERT_EQUALS(HostAndPort(element.second).toString(), found->second.toString());
            }

            {
                const auto found =
                    horizon.getReverseHostMappings().find(HostAndPort(element.second).host());
                ASSERT_TRUE(found != end(horizon.getReverseHostMappings()));
                ASSERT_EQUALS(element.first, found->second);
            }
        }
        ASSERT_EQUALS(input.forwardMapping.size(), horizon.getForwardMappings().size());
        ASSERT_EQUALS(input.forwardMapping.size(), horizon.getReverseHostMappings().size());
    }
}

TEST(SplitHorizonTesting, BSONConstruction) {
    // The none-case can be tested outside ot the table, to help keep the table ctors
    // easier.
    {
        const SplitHorizon horizon(HostAndPort(matchingHostAndPort), boost::none);

        {
            const auto forwardFound = horizon.getForwardMappings().find("__default");
            ASSERT_TRUE(forwardFound != end(horizon.getForwardMappings()));
            ASSERT_EQUALS(forwardFound->second, HostAndPort(matchingHostAndPort));
            ASSERT_EQUALS(horizon.getForwardMappings().size(), std::size_t{1});
        }

        {
            const auto reverseFound = horizon.getReverseHostMappings().find(matchingHost);
            ASSERT_TRUE(reverseFound != end(horizon.getReverseHostMappings()));
            ASSERT_EQUALS(reverseFound->second, "__default");

            ASSERT_EQUALS(horizon.getReverseHostMappings().size(), std::size_t{1});
        }
    }

    const struct {
        BSONObj bsonContents;
        std::string host;
        std::vector<std::pair<std::string, std::string>> expectedMapping;  // bidirectional
        ErrorCodes::Error expectedErrorCode;
        std::vector<std::string> expectedErrorMessageFragments;
        std::vector<std::string> absentErrorMessageFragments;
    } tests[] = {
        // Empty bson object
        {BSONObj(),
         defaultHostAndPort,
         {},
         ErrorCodes::BadValue,
         {"horizons field cannot be empty, if present"},
         {"example.com"}},

        // One simple horizon case.
        {BSON("horizon" << matchingHostAndPort),
         defaultHostAndPort,
         {{"__default", defaultHostAndPort}, {"horizon", matchingHostAndPort}},
         ErrorCodes::OK,
         {},
         {}},

        // Two simple horizons case
        {BSON("horizon" << matchingHostAndPort << "horizon2" << nonmatchingHostAndPort),
         defaultHostAndPort,
         {{"__default", defaultHostAndPort},
          {"horizon", matchingHostAndPort},
          {"horizon2", nonmatchingHostAndPort}},
         ErrorCodes::OK,
         {},
         {}},

        // Three horizons, two having duplicate names
        {
            BSON("duplicateHorizon"
                 << "horizon1.example.com:42"
                 << "duplicateHorizon"
                 << "horizon2.example.com:42"
                 << "uniqueHorizon"
                 << "horizon3.example.com:42"),
            defaultHostAndPort,
            {},
            ErrorCodes::BadValue,
            {"Duplicate horizon name found", "duplicateHorizon"},
            {"uniqueHorizon", "__default"}},

        // Two horizons with duplicate host and ports.
        {BSON("horizonWithDuplicateHost1" << matchingHostAndPort << "horizonWithDuplicateHost2"
                                          << matchingHostAndPort
                                          << "uniqueHorizon"
                                          << nonmatchingHost),
         defaultHostAndPort,
         {},
         ErrorCodes::BadValue,
         {"Duplicate horizon member found", matchingHost},
         {"uniqueHorizon", nonmatchingHost, defaultHost}},
    };

    for (const auto& test : tests) {
        const auto testNumber = &test - tests;
        const BSONObj bson = BSON("horizons" << test.bsonContents);
        const auto& expectedErrorCode = test.expectedErrorCode;

        const auto horizonOpt = [&]() -> boost::optional<SplitHorizon> {
            const auto host = HostAndPort(test.host);
            const auto& bsonElement = bson.firstElement();
            try {
                return SplitHorizon(host, bsonElement);
            } catch (const DBException& ex) {
                ASSERT_NOT_EQUALS(expectedErrorCode, ErrorCodes::OK)
                    << "Failing on test case # " << (&test - tests)
                    << " with unexpected failure: " << ex.toStatus().reason();
                ASSERT_EQUALS(ex.toStatus().code(), expectedErrorCode)
                    << "Failing status code comparison on test case " << testNumber
                    << " reason: " << ex.toStatus().reason();
                for (const auto& fragment : test.expectedErrorMessageFragments) {
                    ASSERT_NOT_EQUALS(ex.toStatus().reason().find(fragment), std::string::npos)
                        << "Wanted to see the text fragment \"" << fragment
                        << "\" in the message: \"" << ex.toStatus().reason() << "\"";
                }
                for (const auto& fragment : test.absentErrorMessageFragments) {
                    ASSERT_EQUALS(ex.toStatus().reason().find(fragment), std::string::npos);
                }
                return boost::none;
            }
        }();

        if (!horizonOpt)
            continue;
        ASSERT_EQUALS(expectedErrorCode, ErrorCodes::OK);

        const auto& horizon = *horizonOpt;

        for (const auto& element : test.expectedMapping) {
            {
                const auto found = horizon.getForwardMappings().find(element.first);
                ASSERT_TRUE(found != end(horizon.getForwardMappings()));
                ASSERT_EQUALS(HostAndPort(element.second).toString(), found->second.toString());
            }

            {
                const auto found =
                    horizon.getReverseHostMappings().find(HostAndPort(element.second).host());
                ASSERT_TRUE(found != end(horizon.getReverseHostMappings()))
                    << "Failed test # " << testNumber
                    << " because we didn't find a reverse mapping for the host " << element.first;
                ASSERT_EQUALS(element.first, found->second) << "on test " << testNumber;
            }
        }

        ASSERT_EQUALS(test.expectedMapping.size(), horizon.getForwardMappings().size());
        ASSERT_EQUALS(test.expectedMapping.size(), horizon.getReverseHostMappings().size());
    }
}

TEST(SplitHorizonTesting, toBSON) {
    struct Input {
        SplitHorizon::ForwardMapping forwardMapping;  // Will get "__default" added to it.

        Input(const MappingType& mapping) : forwardMapping(populateForwardMap(mapping)) {}
    };

    const Input tests[] = {
        {{}},
        {{{"horizon1", "horizon1.example.com:42"}}},
        {{{"horizon1", "horizon1.example.com:42"}, {"horizon2", "horizon2.example.com:42"}}},
        {{{"horizon1", "horizon1.example.com:42"},
          {"horizon2", "horizon2.example.com:42"},
          {"horizon3", "horizon3.example.com:99"}}},
    };
    for (const auto& test : tests) {
        const auto testNumber = &test - tests;
        const auto& input = test;
        const auto& expectedKeys = [&] {
            auto rv = input.forwardMapping;
            rv.erase(SplitHorizon::kDefaultHorizon);
            return rv;
        }();

        const SplitHorizon horizon(input.forwardMapping);

        const BSONObj output = [&] {
            BSONObjBuilder outputBuilder;
            horizon.toBSON(outputBuilder);
            return outputBuilder.obj();
        }();

        if (expectedKeys.empty()) {
            ASSERT_TRUE(output["horizons"].eoo());
            continue;
        }

        ASSERT_FALSE(output["horizons"].eoo());

        for (const auto& horizons : output) {
            ASSERT_TRUE(horizons.fieldNameStringData() == "horizons")
                << "Test #" << testNumber << " Failing finding a horizons element";
        }

        const auto& horizonsElement = output["horizons"];
        ASSERT_EQUALS(horizonsElement.type(), Object);
        const auto& horizons = horizonsElement.Obj();

        std::vector<std::string> visitedHorizons;

        for (const auto& element : horizons) {
            ASSERT_TRUE(expectedKeys.count(element.fieldNameStringData().toString()))
                << "Test #" << testNumber << " Failing because we found a horizon with the name "
                << element.fieldNameStringData() << " which shouldn't exist.";

            ASSERT_TRUE(
                expectedKeys.find(element.fieldNameStringData().toString())->second.toString() ==
                element.valueStringData())
                << "Test #" << testNumber << " failing because horizon "
                << element.fieldNameStringData() << " had an incorrect HostAndPort";
            visitedHorizons.push_back(element.fieldNameStringData().toString());
        }

        ASSERT_EQUALS(visitedHorizons.size(), expectedKeys.size());
    }
}

TEST(SplitHorizonTesting, BSONRoundTrip) {
    struct Input {
        SplitHorizon::ForwardMapping forwardMapping;  // Will get "__default" added to it.

        Input(const MappingType& mapping) : forwardMapping(populateForwardMap(mapping)) {}
    };

    const Input tests[] = {
        {{{"horizon1", "horizon1.example.com:42"}}},
        {{{"horizon1", "horizon1.example.com:42"}, {"horizon2", "horizon2.example.com:42"}}},
    };
    for (const auto& input : tests) {
        const auto testNumber = &input - tests;

        const SplitHorizon horizon(input.forwardMapping);

        const BSONObj bson = [&] {
            BSONObjBuilder outputBuilder;
            horizon.toBSON(outputBuilder);
            return outputBuilder.obj();
        }();

        const SplitHorizon witness(HostAndPort(defaultHostAndPort), bson["horizons"]);

        ASSERT_TRUE(horizon.getForwardMappings() == witness.getForwardMappings())
            << "Test #" << testNumber << " Failed on bson round trip with forward map";
        ASSERT_TRUE(horizon.getReverseHostMappings() == witness.getReverseHostMappings())
            << "Test #" << testNumber << " Failed on bson round trip with reverse map";
    }
}
}  // namespace
}  // namespace repl
}  // namespace mongo
