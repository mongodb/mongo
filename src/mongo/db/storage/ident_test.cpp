/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/storage/ident.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {
const DatabaseName kTestDB = DatabaseName::createDatabaseName_forTest(boost::none, "test"_sd);

/*
 *
 * Tests different forms of generated idents.
 */
class IdentGenerationTest : public unittest::Test {
protected:
    // Asserts the 'ident' is a 'collection' ident as opposed to a specialized table
    // ident such as an 'internal' ident or orphaned ident.
    void assertIsCollectionIdent(const std::string& ident) {
        ASSERT_TRUE(ident::isCollectionIdent(ident)) << "ident: " << ident;
        ASSERT_TRUE(ident::isCollectionOrIndexIdent(ident)) << "ident: " << ident;

        ASSERT_FALSE(ident::isInternalIdent(ident)) << "ident: " << ident;
        ASSERT_FALSE(ident::isInternalIdent(ident, "arbitraryInternalIdentStem"_sd))
            << "ident: " << ident;
    }

    void assertIsIndexIdent(const std::string& ident) {
        ASSERT_FALSE(ident::isCollectionIdent(ident)) << "ident: " << ident;
        ASSERT_TRUE(ident::isCollectionOrIndexIdent(ident)) << "ident: " << ident;

        ASSERT_FALSE(ident::isInternalIdent(ident)) << "ident: " << ident;
        ASSERT_FALSE(ident::isInternalIdent(ident, "arbitraryInternalIdentStem"_sd))
            << "ident: " << ident;
    }
};

TEST_F(IdentGenerationTest, CollectionIdentsAreUnique) {
    // Tests that 'generateNewCollectionIdent()' yields unique idents when run twice with the same
    // 'directoryPerDB' and 'directoryForIndexes' parameters.
    const auto testGeneratesUniqueIdents = [&](bool directoryPerDB, bool directoryForIndexes) {
        const auto identRun0 =
            ident::generateNewCollectionIdent(kTestDB, directoryPerDB, directoryForIndexes);
        const auto identRun1 =
            ident::generateNewCollectionIdent(kTestDB, directoryPerDB, directoryForIndexes);
        ASSERT_NE(identRun0, identRun1) << "directoryPerDB: " << directoryPerDB
                                        << ", directoryForIndexes: " << directoryForIndexes;
    };

    testGeneratesUniqueIdents(false /* directoryPerDB */, false /* directoryForIndexes */);
    testGeneratesUniqueIdents(true /* directoryPerDB */, false /* directoryForIndexes */);
    testGeneratesUniqueIdents(false /* directoryPerDB */, true /* directoryForIndexes */);
    testGeneratesUniqueIdents(true /* directoryPerDB */, true /* directoryForIndexes */);
}

TEST_F(IdentGenerationTest, CollectionIdentsYieldIsCollectionIdentTrue) {
    const auto identDefault = ident::generateNewCollectionIdent(
        kTestDB, false /* directoryPerDB */, false /* directoryForIndexes */);
    assertIsCollectionIdent(identDefault);

    const auto identDirectoryPerDB = ident::generateNewCollectionIdent(
        kTestDB, true /* directoryPerDB */, false /* directoryForIndexes */);
    assertIsCollectionIdent(identDirectoryPerDB);

    const auto identDirectoryForIndexes = ident::generateNewCollectionIdent(
        kTestDB, false /* directoryPerDB */, true /* directoryForIndexes */);
    assertIsCollectionIdent(identDirectoryForIndexes);

    const auto identDirectoryPerDBAndIndexes = ident::generateNewCollectionIdent(
        kTestDB, false /* directoryPerDB */, true /* directoryForIndexes */);
    assertIsCollectionIdent(identDirectoryPerDBAndIndexes);
}

TEST_F(IdentGenerationTest, LegacyCollectionIdentsYieldIsCollectionIdentTrue) {
    // Idents generated before v8.2 are made unique with a <counter> + <random number> combination.
    // Whereas modern idents are generated with a UUID suffix to enforce uniqueness.
    //
    // Test that legacy collection idents are classified correctly.

    // Neither the legacy equivalent of 'directoryPerDB' nor 'directoryForIndexes' are set.
    std::string legacyIdentDefault = "collection-9-11733751379908443489";
    assertIsCollectionIdent(legacyIdentDefault);

    // 'directoryPerDB' equivalent set.
    std::string legacyIdentDirectoryPerDb = "testDB/collection-9-11733751379908443489";
    assertIsCollectionIdent(legacyIdentDirectoryPerDb);

    // 'directoryForIndexes' equivalent set.
    std::string legacyIdentDirectoryForIndexes = "collection/9-11733751379908443489";
    assertIsCollectionIdent(legacyIdentDirectoryForIndexes);

    // Both the 'directoryPerDB' and 'directoryForIndexes' equivalent flags set.
    std::string legacyIdentDirectoryPerDBAndIndexes = "testDB/collection/9-11733751379908443489";
    assertIsCollectionIdent(legacyIdentDirectoryPerDBAndIndexes);
}

TEST_F(IdentGenerationTest, IndexIdentsAreUnique) {
    // Tests that 'generateNewIndexIdent()' yields unique idents when run twice with the same
    // 'directoryPerDB' and 'directoryForIndexes' parameters.
    const auto testGeneratesUniqueIdents = [&](bool directoryPerDB, bool directoryForIndexes) {
        const auto identRun0 =
            ident::generateNewIndexIdent(kTestDB, directoryPerDB, directoryForIndexes);
        const auto identRun1 =
            ident::generateNewIndexIdent(kTestDB, directoryPerDB, directoryForIndexes);
        ASSERT_NE(identRun0, identRun1) << "directoryPerDB: " << directoryPerDB
                                        << ", directoryForIndexes: " << directoryForIndexes;
    };

    testGeneratesUniqueIdents(false /* directoryPerDB */, false /* directoryForIndexes */);
    testGeneratesUniqueIdents(true /* directoryPerDB */, false /* directoryForIndexes */);
    testGeneratesUniqueIdents(false /* directoryPerDB */, true /* directoryForIndexes */);
    testGeneratesUniqueIdents(true /* directoryPerDB */, true /* directoryForIndexes */);
}

TEST_F(IdentGenerationTest, IndexIdentsYieldIsIndexIdentTrue) {
    const auto identDefault = ident::generateNewIndexIdent(
        kTestDB, false /* directoryPerDB */, false /* directoryForIndexes */);
    assertIsIndexIdent(identDefault);

    const auto identDirectoryPerDB = ident::generateNewIndexIdent(
        kTestDB, true /* directoryPerDB */, false /* directoryForIndexes */);
    assertIsIndexIdent(identDirectoryPerDB);

    const auto identDirectoryForIndexes = ident::generateNewIndexIdent(
        kTestDB, false /* directoryPerDB */, true /* directoryForIndexes */);
    assertIsIndexIdent(identDirectoryForIndexes);

    const auto identDirectoryPerDBAndIndexes = ident::generateNewIndexIdent(
        kTestDB, false /* directoryPerDB */, true /* directoryForIndexes */);
    assertIsIndexIdent(identDirectoryPerDBAndIndexes);
}

TEST_F(IdentGenerationTest, LegacyIndexIdentsYieldIsIndexIdentTrue) {
    // Idents generated before v8.2 are made unique with a <counter> + <random number> combination.
    // Whereas modern idents are generated with a UUID suffix to enforce uniqueness.
    //
    // Test that legacy index idents are classified correctly.

    // Neither the legacy equivalent of 'directoryPerDB' nor 'directoryForIndexes' are set.
    std::string legacyIdentDefault = "index-9-11733751379908443489";
    assertIsIndexIdent(legacyIdentDefault);

    // 'directoryPerDB' equivalent set.
    std::string legacyIdentDirectoryPerDb = "testDB/index-9-11733751379908443489";
    assertIsIndexIdent(legacyIdentDirectoryPerDb);

    // 'directoryForIndexes' equivalent set.
    std::string legacyIdentDirectoryForIndexes = "index/9-11733751379908443489";
    assertIsIndexIdent(legacyIdentDirectoryForIndexes);

    // Both the 'directoryPerDB' and 'directoryForIndexes' equivalent flags set.
    std::string legacyIdentDirectoryPerDBAndIndexes = "testDB/index/9-11733751379908443489";
    assertIsIndexIdent(legacyIdentDirectoryPerDBAndIndexes);
}

TEST_F(IdentGenerationTest, InternalIdentsAreGeneratedAndClassifiedCorrectly) {
    // By default, an internal ident is generated with an empty ident stem.
    const auto defaultInternalIdent = ident::generateNewInternalIdent();
    ASSERT_TRUE(ident::isInternalIdent(defaultInternalIdent)) << "ident: " << defaultInternalIdent;

    // Validate 'generateNewInternalIdent()' yields a unique internal ident each time.
    const auto defaultInternalIdent2 = ident::generateNewInternalIdent();
    ASSERT_NE(defaultInternalIdent, defaultInternalIdent2);
    ASSERT_TRUE(ident::isInternalIdent(defaultInternalIdent2))
        << "ident: " << defaultInternalIdent2;

    const auto specificIdentStem = "specificInternalIdentCategory"_sd;

    // 'defaultInternalIdent' wasn't generated with 'specificIdentStem', and doesn't fall under the
    // same ident category as internal idents with 'specificIdentStem'.
    ASSERT_FALSE(ident::isInternalIdent(defaultInternalIdent, specificIdentStem))
        << "ident: " << defaultInternalIdent;

    const auto internalIdentWithStem = ident::generateNewInternalIdent(specificIdentStem);
    ASSERT_TRUE(ident::isInternalIdent(internalIdentWithStem))
        << "ident: " << internalIdentWithStem;
    ASSERT_TRUE(ident::isInternalIdent(internalIdentWithStem, specificIdentStem))
        << "ident: " << internalIdentWithStem;
}
}  // namespace
}  // namespace mongo
