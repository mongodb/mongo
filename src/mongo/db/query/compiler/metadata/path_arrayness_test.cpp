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

#include "mongo/db/query/compiler/metadata/path_arrayness.h"

#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/metadata/path_arrayness_test_helpers.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(PathArraynessTest, InsertIntoPathArraynessPredefined) {
    // Array: ["a"]
    FieldPath field_A("a");
    MultikeyComponents multikeyPaths_A{0U};

    // Array: ["a", "a.b.c"]
    FieldPath field_ABC("a.b.c");
    MultikeyComponents multikeyPaths_ABC{{0U, 2U}};

    // Array: ["a", "a.b.d"]
    FieldPath field_ABD("a.b.d");
    MultikeyComponents multikeyPaths_ABD{{0U, 2U}};

    // Array: ["a", "a.b.c"]
    FieldPath field_ABCJ("a.b.c.j");
    MultikeyComponents multikeyPaths_ABCJ{{0U, 2U}};

    // Array: ["a", "a.b.d"]
    FieldPath field_ABDE("a.b.d.e");
    MultikeyComponents multikeyPaths_ABDE{0U, 2U};

    // Array: []
    FieldPath field_BDE("b.d.e");
    MultikeyComponents multikeyPaths_BDE{};

    // Array: ["b.d.e.f"] (Only final component is array)
    FieldPath field_BDEF("b.d.e.f");
    MultikeyComponents multikeyPaths_BDEF{3U};

    std::vector<FieldPath> fields{
        field_A, field_ABC, field_ABD, field_ABCJ, field_ABDE, field_BDE, field_BDEF};
    std::vector<MultikeyComponents> multikeyness{multikeyPaths_A,
                                                 multikeyPaths_ABC,
                                                 multikeyPaths_ABD,
                                                 multikeyPaths_ABCJ,
                                                 multikeyPaths_ABDE,
                                                 multikeyPaths_BDE,
                                                 multikeyPaths_BDEF};

    auto pathsToInsert = combineVectors(fields, multikeyness);
    auto arraynessMapInitial = tranformVectorToMap(pathsToInsert);

    PathArrayness pathArrayness;
    for (auto&& [fieldPath, multikey] : pathsToInsert) {
        pathArrayness.addPath(fieldPath, multikey);
    }

    auto arraynessMapExported = pathArrayness.exportToMap_forTest();

    ASSERT_EQ(arraynessMapInitial, arraynessMapExported);
    ASSERT_EQ(pathArrayness.isPathArray(field_A), true);
    ASSERT_EQ(pathArrayness.isPathArray(field_ABC), true);
    ASSERT_EQ(pathArrayness.isPathArray(field_ABD), true);
    ASSERT_EQ(pathArrayness.isPathArray(field_ABCJ), true);
    ASSERT_EQ(pathArrayness.isPathArray(field_ABDE), true);
    ASSERT_EQ(pathArrayness.isPathArray(field_BDE), false);
    ASSERT_EQ(pathArrayness.isPathArray(field_BDEF), true);
}

TEST(PathArraynessTest, InsertIntoPathArraynessGenerated) {
    size_t seed = 1354754;
    size_t seed2 = 3421354754;

    // Number of paths to insert.
    int numberOfPaths = 10;

    // Number of distinct lengths of paths.
    // by default we chose that we have 5 field paths for each length.
    auto ndvLengths = numberOfPaths / 5;
    // Maximum length of dotted field paths.
    int maxLength = 100;

    std::vector<std::pair<std::string, MultikeyComponents>> pathsToInsert =
        generateRandomFieldPathsWithArraynessInfo(
            numberOfPaths, maxLength, ndvLengths, seed, seed2);

    auto arraynessMapInitial = tranformVectorToMap(pathsToInsert);

    PathArrayness pathArrayness;
    for (auto&& [fieldPath, multikey] : pathsToInsert) {
        pathArrayness.addPath(fieldPath, multikey);
    }

    auto arraynessMapExported = pathArrayness.exportToMap_forTest();

    ASSERT_EQ(arraynessMapInitial, arraynessMapExported);
}

TEST(PathArraynessTest, BuildAndLookupNonExistingFields) {
    // Array: ["a", "a.b", "a.b.c"]
    FieldPath field_ABC("a.b.c");
    MultikeyComponents multikeyPaths_ABC{0U, 1U, 2U};

    // We will not insert "a.b.c.d" into the trie.
    FieldPath field_ABCD("a.b.c.d");

    std::vector<FieldPath> fields{field_ABC};
    std::vector<MultikeyComponents> multikeyness{multikeyPaths_ABC};

    PathArrayness pathArrayness;

    for (size_t i = 0; i < fields.size(); i++) {
        pathArrayness.addPath(fields[i], multikeyness[i]);
    }

    ASSERT_EQ(pathArrayness.isPathArray(field_ABC), true);

    // Path component "d" does not exist but it has prefix "a", "a.b" and "a.b.c" that are arrays.
    ASSERT_EQ(pathArrayness.isPathArray(field_ABCD), true);
}

TEST(PathArraynessTest, LookupEmptyTrie) {
    // We will not insert any fields into the trie.
    FieldPath field_A("a");
    FieldPath field_ABCD("a.b.c.d");
    std::vector<FieldPath> fields{field_A, field_ABCD};

    PathArrayness pathArrayness;

    // Neither of these fields or their prefixes are in the trie, so assume arrays.
    ASSERT_EQ(pathArrayness.isPathArray(field_A), true);
    ASSERT_EQ(pathArrayness.isPathArray(field_ABCD), true);
}

TEST(ArraynessTrie, BuildAndLookupTrieWithConflictingArrayInformation) {
    // Array: ["a.b"]
    FieldPath field_AB("a.b");
    MultikeyComponents multikeyPaths_AB{1U};

    // Array: ["a.b.c"]. Note in this case "a.b" is not an array.
    FieldPath field_ABC("a.b.c");
    MultikeyComponents multikeyPaths_ABC{2U};
    // First add field_AB to trie and then add field_ABC to trie.
    std::vector<FieldPath> fields{field_AB, field_ABC};
    std::vector<MultikeyComponents> multikeyness{multikeyPaths_AB, multikeyPaths_ABC};

    PathArrayness pathArrayness;

    pathArrayness.addPath(field_AB, multikeyPaths_AB);
    pathArrayness.addPath(field_ABC, multikeyPaths_ABC);

    ASSERT_EQ(pathArrayness.isPathArray(field_AB), false);

    // Now let's flip the insertion order.
    PathArrayness pathArrayness1;
    pathArrayness1.addPath(field_ABC, multikeyPaths_ABC);
    pathArrayness1.addPath(field_AB, multikeyPaths_AB);

    // We should still get the same result.
    ASSERT_EQ(pathArrayness1.isPathArray(field_AB), false);
}

TEST(ArraynessTrie, BuildAndLookupTrieWithSameArrayInformation) {
    // Array: ["a.b"]
    FieldPath field_AB("a.b");
    MultikeyComponents multikeyPaths_AB{1U};

    // Array: ["a.b"]. Note in this case "a.b" is not an array.
    FieldPath field_ABC("a.b.c");
    MultikeyComponents multikeyPaths_ABC{1U};

    // Note "a" is not an array in field_AB or field_ABC examples above.
    FieldPath field_A("a");

    // First add field_AB to trie and then add field_ABC to trie.
    std::vector<FieldPath> fields{field_AB, field_ABC};
    std::vector<MultikeyComponents> multikeyness{multikeyPaths_AB, multikeyPaths_ABC};

    PathArrayness pathArrayness;

    pathArrayness.addPath(field_AB, multikeyPaths_AB);
    pathArrayness.addPath(field_ABC, multikeyPaths_ABC);

    // Path "a.b" is an array in both of the paths inserted into the trie.
    ASSERT_EQ(pathArrayness.isPathArray(field_AB), true);
    // Path "a" is not an array in either of the paths inserted into the trie.
    ASSERT_EQ(pathArrayness.isPathArray(field_A), false);
}
}  // namespace mongo
