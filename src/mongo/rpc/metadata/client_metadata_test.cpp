/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/rpc/metadata/client_metadata.h"

#include <boost/filesystem.hpp>
#include <map>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/s/is_mongos.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

constexpr auto kMetadataDoc = "client"_sd;
constexpr auto kApplication = "application"_sd;
constexpr auto kDriver = "driver"_sd;
constexpr auto kName = "name"_sd;
constexpr auto kType = "type"_sd;
constexpr auto kVersion = "version"_sd;
constexpr auto kOperatingSystem = "os"_sd;
constexpr auto kArchitecture = "architecture"_sd;
constexpr auto kMongos = "mongos"_sd;
constexpr auto kClient = "client"_sd;
constexpr auto kHost = "host"_sd;

constexpr auto kUnknown = "unkown"_sd;

#define ASSERT_DOC_OK(...)                                                                \
    do {                                                                                  \
        auto _swParseStatus =                                                             \
            ClientMetadata::parse(BSON(kMetadataDoc << BSON(__VA_ARGS__))[kMetadataDoc]); \
        ASSERT_OK(_swParseStatus.getStatus());                                            \
    } while (0)

#define ASSERT_DOC_NOT_OK(...)                                                            \
    do {                                                                                  \
        auto _swParseStatus =                                                             \
            ClientMetadata::parse(BSON(kMetadataDoc << BSON(__VA_ARGS__))[kMetadataDoc]); \
        ASSERT_NOT_OK(_swParseStatus.getStatus());                                        \
    } while (0)


TEST(ClientMetadatTest, TestLoopbackTest) {
    // Serialize without application name
    {
        BSONObjBuilder builder;
        ASSERT_OK(ClientMetadata::serializePrivate("a", "b", "c", "d", "e", "f", "g", &builder));

        auto obj = builder.obj();
        auto swParseStatus = ClientMetadata::parse(obj[kMetadataDoc]);
        ASSERT_OK(swParseStatus.getStatus());
        ASSERT_EQUALS("g", swParseStatus.getValue().get().getApplicationName());

        BSONObj outDoc =
            BSON(kMetadataDoc << BSON(
                     kApplication << BSON(kName << "g") << kDriver
                                  << BSON(kName << "a" << kVersion << "b")
                                  << kOperatingSystem
                                  << BSON(kType << "c" << kName << "d" << kArchitecture << "e"
                                                << kVersion
                                                << "f")));
        ASSERT_BSONOBJ_EQ(obj, outDoc);
    }

    // Serialize without application name
    {
        BSONObjBuilder builder;
        ClientMetadata::serializePrivate("a", "b", "c", "d", "e", "f", &builder);

        auto obj = builder.obj();
        auto swParseStatus = ClientMetadata::parse(obj[kMetadataDoc]);
        ASSERT_OK(swParseStatus.getStatus());

        BSONObj outDoc = BSON(
            kMetadataDoc << BSON(
                kDriver << BSON(kName << "a" << kVersion << "b") << kOperatingSystem
                        << BSON(kType << "c" << kName << "d" << kArchitecture << "e" << kVersion
                                      << "f")));
        ASSERT_BSONOBJ_EQ(obj, outDoc);
    }

    // Serialize with the os information automatically computed
    {
        BSONObjBuilder builder;
        ASSERT_OK(ClientMetadata::serialize("a", "b", "f", &builder));

        auto obj = builder.obj();

        auto swParse = ClientMetadata::parse(obj[kMetadataDoc]);
        ASSERT_OK(swParse.getStatus());
        ASSERT_EQUALS("f", swParse.getValue().get().getApplicationName());
    }
}

// Mixed: no client metadata document
TEST(ClientMetadatTest, TestEmptyDoc) {
    {
        auto parseStatus = ClientMetadata::parse(BSONElement());

        ASSERT_OK(parseStatus.getStatus());
    }

    {
        auto obj = BSON("client" << BSONObj());
        auto parseStatus = ClientMetadata::parse(obj[kMetadataDoc]);

        ASSERT_NOT_OK(parseStatus.getStatus());
    }
}

// Positive: test with only required fields
TEST(ClientMetadatTest, TestRequiredOnlyFields) {
    // Without app name
    ASSERT_DOC_OK(kDriver << BSON(kName << "n1" << kVersion << "v1") << kOperatingSystem
                          << BSON(kType << kUnknown));

    // With AppName
    ASSERT_DOC_OK(kApplication << BSON(kName << "1") << kDriver
                               << BSON(kName << "n1" << kVersion << "v1")
                               << kOperatingSystem
                               << BSON(kType << kUnknown));
}


// Positive: test with app_name spelled wrong fields
TEST(ClientMetadatTest, TestWithAppNameSpelledWrong) {
    ASSERT_DOC_OK(kApplication << BSON("extra"
                                       << "1")
                               << kDriver
                               << BSON(kName << "n1" << kVersion << "v1")
                               << kOperatingSystem
                               << BSON(kType << kUnknown));
}

// Positive: test with empty application document
TEST(ClientMetadatTest, TestWithEmptyApplication) {
    ASSERT_DOC_OK(kApplication << BSONObj() << kDriver << BSON(kName << "n1" << kVersion << "v1")
                               << kOperatingSystem
                               << BSON(kType << kUnknown));
}

// Negative: test with appplication wrong type
TEST(ClientMetadatTest, TestNegativeWithAppNameWrongType) {
    ASSERT_DOC_NOT_OK(kApplication << "1" << kDriver << BSON(kName << "n1" << kVersion << "v1")
                                   << kOperatingSystem
                                   << BSON(kType << kUnknown));
}

// Positive: test with extra fields
TEST(ClientMetadatTest, TestExtraFields) {
    ASSERT_DOC_OK(kApplication << BSON(kName << "1"
                                             << "extra"
                                             << "v1")
                               << kDriver
                               << BSON(kName << "n1" << kVersion << "v1")
                               << kOperatingSystem
                               << BSON(kType << kUnknown));
    ASSERT_DOC_OK(kApplication << BSON(kName << "1"
                                             << "extra"
                                             << "v1")
                               << kDriver
                               << BSON(kName << "n1" << kVersion << "v1"
                                             << "extra"
                                             << "v1")
                               << kOperatingSystem
                               << BSON(kType << kUnknown));
    ASSERT_DOC_OK(kApplication << BSON(kName << "1"
                                             << "extra"
                                             << "v1")
                               << kDriver
                               << BSON(kName << "n1" << kVersion << "v1")
                               << kOperatingSystem
                               << BSON(kType << kUnknown << "extra"
                                             << "v1"));
    ASSERT_DOC_OK(kApplication << BSON(kName << "1"
                                             << "extra"
                                             << "v1")
                               << kDriver
                               << BSON(kName << "n1" << kVersion << "v1")
                               << kOperatingSystem
                               << BSON(kType << kUnknown)
                               << "extra"
                               << "v1");
}

// Negative: only application specified
TEST(ClientMetadatTest, TestNegativeOnlyApplication) {
    ASSERT_DOC_NOT_OK(kApplication << BSON(kName << "1"
                                                 << "extra"
                                                 << "v1"));
}

// Negative: all combinations of only missing 1 required field
TEST(ClientMetadatTest, TestNegativeMissingRequiredOneField) {
    ASSERT_DOC_NOT_OK(kDriver << BSON(kVersion << "v1") << kOperatingSystem
                              << BSON(kType << kUnknown));
    ASSERT_DOC_NOT_OK(kDriver << BSON(kName << "n1") << kOperatingSystem
                              << BSON(kType << kUnknown));
    ASSERT_DOC_NOT_OK(kDriver << BSON(kName << "n1" << kVersion << "v1"));
}

// Negative: document with wrong types for required fields
TEST(ClientMetadatTest, TestNegativeWrongTypes) {
    ASSERT_DOC_NOT_OK(kApplication << BSON(kName << 1) << kDriver
                                   << BSON(kName << "n1" << kVersion << "v1")
                                   << kOperatingSystem
                                   << BSON(kType << kUnknown));
    ASSERT_DOC_NOT_OK(kApplication << BSON(kName << "1") << kDriver
                                   << BSON(kName << 1 << kVersion << "v1")
                                   << kOperatingSystem
                                   << BSON(kType << kUnknown));
    ASSERT_DOC_NOT_OK(kApplication << BSON(kName << "1") << kDriver
                                   << BSON(kName << "n1" << kVersion << 1)
                                   << kOperatingSystem
                                   << BSON(kType << kUnknown));
    ASSERT_DOC_NOT_OK(kApplication << BSON(kName << "1") << kDriver
                                   << BSON(kName << "n1" << kVersion << "v1")
                                   << kOperatingSystem
                                   << BSON(kType << 1));
}

// Negative: document larger than 512 bytes
TEST(ClientMetadatTest, TestNegativeLargeDocument) {
    bool savedMongos = isMongos();
    auto unsetMongoS = MakeGuard(&setMongos, savedMongos);

    setMongos(true);
    {
        std::string str(350, 'x');
        ASSERT_DOC_OK(kApplication << BSON(kName << "1") << kDriver
                                   << BSON(kName << "n1" << kVersion << "1")
                                   << kOperatingSystem
                                   << BSON(kType << kUnknown)
                                   << "extra"
                                   << str);
    }
    {
        std::string str(512, 'x');
        ASSERT_DOC_NOT_OK(kApplication << BSON(kName << "1") << kDriver
                                       << BSON(kName << "n1" << kVersion << "1")
                                       << kOperatingSystem
                                       << BSON(kType << kUnknown)
                                       << "extra"
                                       << str);
    }
}

// Negative: document with app_name larger than 128 bytes
TEST(ClientMetadatTest, TestNegativeLargeAppName) {
    {
        std::string str(128, 'x');
        ASSERT_DOC_OK(kApplication << BSON(kName << str) << kDriver
                                   << BSON(kName << "n1" << kVersion << "1")
                                   << kOperatingSystem
                                   << BSON(kType << kUnknown));

        BSONObjBuilder builder;
        ASSERT_OK(ClientMetadata::serialize("n1", "1", str, &builder));
    }
    {
        std::string str(129, 'x');
        ASSERT_DOC_NOT_OK(kApplication << BSON(kName << str) << kDriver
                                       << BSON(kName << "n1" << kVersion << "1")
                                       << kOperatingSystem
                                       << BSON(kType << kUnknown));

        BSONObjBuilder builder;
        ASSERT_NOT_OK(ClientMetadata::serialize("n1", "1", str, &builder));
    }
}

// Serialize and attach mongos information
TEST(ClientMetadatTest, TestMongoSAppend) {
    BSONObjBuilder builder;
    ASSERT_OK(ClientMetadata::serializePrivate("a", "b", "c", "d", "e", "f", "g", &builder));

    auto obj = builder.obj();
    auto swParseStatus = ClientMetadata::parse(obj[kMetadataDoc]);
    ASSERT_OK(swParseStatus.getStatus());
    ASSERT_EQUALS("g", swParseStatus.getValue().get().getApplicationName());

    swParseStatus.getValue().get().setMongoSMetadata("h", "i", "j");
    auto doc = swParseStatus.getValue().get().getDocument();

    constexpr auto kMongos = "mongos"_sd;
    constexpr auto kClient = "client"_sd;
    constexpr auto kHost = "host"_sd;

    BSONObj outDoc =
        BSON(kApplication << BSON(kName << "g") << kDriver << BSON(kName << "a" << kVersion << "b")
                          << kOperatingSystem
                          << BSON(kType << "c" << kName << "d" << kArchitecture << "e" << kVersion
                                        << "f")
                          << kMongos
                          << BSON(kHost << "h" << kClient << "i" << kVersion << "j"));
    ASSERT_BSONOBJ_EQ(doc, outDoc);
}

}  // namespace mongo
