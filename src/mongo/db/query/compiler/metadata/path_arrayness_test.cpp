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

#include "mongo/bson/bson_depth.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/metadata/path_arrayness_test_helpers.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(PathArraynessTest, InsertIntoPathArraynessPredefined) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();

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
        pathArrayness.addPath(fieldPath, multikey, true);
    }

    auto arraynessMapExported = pathArrayness.exportToMap_forTest();

    ASSERT_EQ(arraynessMapInitial, arraynessMapExported);
    ASSERT_EQ(pathArrayness.isPathArray(field_A, &expCtx), true);
    ASSERT_EQ(pathArrayness.isPathArray(field_ABC, &expCtx), true);
    ASSERT_EQ(pathArrayness.isPathArray(field_ABD, &expCtx), true);
    ASSERT_EQ(pathArrayness.isPathArray(field_ABCJ, &expCtx), true);
    ASSERT_EQ(pathArrayness.isPathArray(field_ABDE, &expCtx), true);
    ASSERT_EQ(pathArrayness.isPathArray(field_BDE, &expCtx), false);
    ASSERT_EQ(pathArrayness.isPathArray(field_BDEF, &expCtx), true);
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
        pathArrayness.addPath(fieldPath, multikey, true);
    }

    auto arraynessMapExported = pathArrayness.exportToMap_forTest();

    ASSERT_EQ(arraynessMapInitial, arraynessMapExported);
}

TEST(PathArraynessTest, BuildAndLookupNonExistingFields) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();

    // Array: ["a", "a.b", "a.b.c"]
    FieldPath field_ABC("a.b.c");
    MultikeyComponents multikeyPaths_ABC{0U, 1U, 2U};

    // We will not insert "a.b.c.d" into the trie.
    FieldPath field_ABCD("a.b.c.d");

    std::vector<FieldPath> fields{field_ABC};
    std::vector<MultikeyComponents> multikeyness{multikeyPaths_ABC};

    PathArrayness pathArrayness;

    for (size_t i = 0; i < fields.size(); i++) {
        pathArrayness.addPath(fields[i], multikeyness[i], true);
    }

    ASSERT_EQ(pathArrayness.isPathArray(field_ABC, &expCtx), true);

    // Path component "d" does not exist but it has prefix "a", "a.b" and "a.b.c" that are
    ASSERT_EQ(pathArrayness.isPathArray(field_ABCD, &expCtx), true);
}

TEST(PathArraynessTest, LookupEmptyTrie) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();

    // We will not insert any fields into the trie.
    FieldPath field_A("a");
    FieldPath field_ABCD("a.b.c.d");
    std::vector<FieldPath> fields{field_A, field_ABCD};

    PathArrayness pathArrayness;

    // Neither of these fields or their prefixes are in the trie, so assume arrays.
    ASSERT_EQ(pathArrayness.isPathArray(field_A, &expCtx), true);
    ASSERT_EQ(pathArrayness.isPathArray(field_ABCD, &expCtx), true);
}

// When fully rebuilding the trie, we err on the side of non-arrayness when conflicts occur.
TEST(ArraynessTrie, FullyRebuildAndLookupTrieWithConflictingArrayInformation) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();

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

    pathArrayness.addPath(field_AB, multikeyPaths_AB, true);
    pathArrayness.addPath(field_ABC, multikeyPaths_ABC, true);

    // In the case of conflicts, we assume multikeyness.
    ASSERT_EQ(pathArrayness.isPathArray(field_AB, &expCtx), false);

    // Now let's flip the insertion order.
    PathArrayness pathArrayness1;
    pathArrayness1.addPath(field_ABC, multikeyPaths_ABC, true);
    pathArrayness1.addPath(field_AB, multikeyPaths_AB, true);

    // We should still get the same result.
    ASSERT_EQ(pathArrayness1.isPathArray(field_AB, &expCtx), false);
}

// When updating the trie due to an index catalog update following a write operation, we err on the
// side of arrayness when conflicts occur.
TEST(ArraynessTrie, UpdateAndLookupTrieWithConflictingArrayInformation) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();

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

    pathArrayness.addPath(field_AB, multikeyPaths_AB, false);
    pathArrayness.addPath(field_ABC, multikeyPaths_ABC, false);

    // In the case of conflicts, we assume multikeyness.
    ASSERT_EQ(pathArrayness.isPathArray(field_AB, &expCtx), true);

    // Now let's flip the insertion order.
    PathArrayness pathArrayness1;
    pathArrayness1.addPath(field_ABC, multikeyPaths_ABC, false);
    pathArrayness1.addPath(field_AB, multikeyPaths_AB, false);

    // We should still get the same result.
    ASSERT_EQ(pathArrayness1.isPathArray(field_AB, &expCtx), true);
}

TEST(ArraynessTrie, BuildAndLookupTrieWithSameArrayInformation) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();

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

    pathArrayness.addPath(field_AB, multikeyPaths_AB, true);
    pathArrayness.addPath(field_ABC, multikeyPaths_ABC, true);

    // Path "a.b" is an array in both of the paths inserted into the trie.
    ASSERT_EQ(pathArrayness.isPathArray(field_AB, &expCtx), true);
    // Path "a" is not an array in either of the paths inserted into the trie.
    ASSERT_EQ(pathArrayness.isPathArray(field_A, &expCtx), false);
}


/**
 * Test that FieldRef conversion to FieldPath is handled correctly
 * Since FieldPath has stricter validation than FieldRef, a path that is valid by FieldRef
 * standards but not by FieldPath standards should not be added to the trie on insert and on
 * lookup should always conservatively return true for arrayness.
 */

// FieldRef allows empty string, but FieldPath does not.
TEST(PathArraynessTest, FieldRefEmptyString) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();

    FieldRef emptyFieldRef("");
    MultikeyComponents multikeyPaths{0U};

    PathArrayness pathArrayness;

    // Lookup should conservatively return true for invalid FieldPath.
    ASSERT_EQ(pathArrayness.isPathArray(emptyFieldRef, &expCtx), true);
}

// FieldRef allows paths ending with a dot, but FieldPath does not.
TEST(PathArraynessTest, FieldRefEndsWithDot) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();

    FieldRef fieldRefWithDot("a.b.");
    MultikeyComponents multikeyPaths{0U};

    PathArrayness pathArrayness;

    // Lookup should conservatively return true for invalid FieldPath.
    ASSERT_EQ(pathArrayness.isPathArray(fieldRefWithDot, &expCtx), true);
}

// FieldRef allows empty field names between dots (e.g., "a..b"), but FieldPath does not.
TEST(PathArraynessTest, FieldRefEmptyFieldNameBetweenDots) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();

    FieldRef fieldRefWithEmpty("a..b");
    MultikeyComponents multikeyPaths{0U};

    PathArrayness pathArrayness;

    // Lookup should conservatively return true for invalid FieldPath.
    ASSERT_EQ(pathArrayness.isPathArray(fieldRefWithEmpty, &expCtx), true);
}

// FieldRef allows up to 255 components, but FieldPath only allows 200.
TEST(PathArraynessTest, FieldRefTooManyComponents) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();

    // Create a path with 201 components (exceeding FieldPath limit).
    std::string longPath = "a";
    for (int i = 0; i < BSONDepth::kDefaultMaxAllowableDepth; ++i) {
        longPath += ".b";
    }
    // Now longPath has 1 + 200 = 201 components, which exceeds the FieldPath limit of 200.

    FieldRef fieldRefLong(longPath);
    MultikeyComponents multikeyPaths{0U};

    PathArrayness pathArrayness;

    // Lookup should conservatively return true for invalid FieldPath.
    ASSERT_EQ(pathArrayness.isPathArray(fieldRefLong, &expCtx), true);
}

// FieldRef allows any field name starting with $, but FieldPath only allows certain
// dollar-prefixed fields (like "$ref", "$id", etc.). A field like "$invalidField" should fail
// FieldPath validation.
TEST(PathArraynessTest, FieldRefInvalidDollarPrefix) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();

    FieldRef fieldRefWithInvalidDollar("a.$invalidField");
    MultikeyComponents multikeyPaths{1U};

    PathArrayness pathArrayness;

    // Lookup should conservatively return true for invalid FieldPath.
    ASSERT_EQ(pathArrayness.isPathArray(fieldRefWithInvalidDollar, &expCtx), true);
}

// Test that a FieldRef that is also a valid FieldPath works correctly.
TEST(PathArraynessTest, FieldRefValidPath) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();

    // Arrays: ["a.b", "a.b.c"]
    std::string validFieldRefString("a.b.c");
    MultikeyComponents multikeyPaths{1U, 2U};

    PathArrayness pathArrayness;
    auto initialState = pathArrayness.exportToMap_forTest();

    // Insert valid path - should succeed and be added to trie.
    pathArrayness.addPath(FieldPath(validFieldRefString), multikeyPaths, true);
    auto afterInsertState = pathArrayness.exportToMap_forTest();

    // Trie should have changed since the path is valid and was added.
    ASSERT_NE(initialState, afterInsertState);
    // Should have "a", "a.b", and "a.b.c" entries (all nodes in the path)
    ASSERT_EQ(afterInsertState.size(), 3U);
    ASSERT_EQ(afterInsertState["a"], false);     // Component 0 is array
    ASSERT_EQ(afterInsertState["a.b"], true);    // Component 1 is array
    ASSERT_EQ(afterInsertState["a.b.c"], true);  // Component 2 is not array

    // Lookup should work correctly - "a.b.c" has array prefix, so it's considered an array.
    FieldRef validFieldRef = FieldRef(validFieldRefString);
    ASSERT_EQ(pathArrayness.isPathArray(validFieldRef, &expCtx), true);

    // Test lookup of a prefix that is an array.
    FieldRef prefixFieldRef("a.b");
    ASSERT_EQ(pathArrayness.isPathArray(prefixFieldRef, &expCtx), true);

    // Test lookup of a prefix that is not an array.
    FieldRef fieldRef("a");
    ASSERT_EQ(pathArrayness.isPathArray(fieldRef, &expCtx), false);
}

/**
 * Test that the enablePathArrayness query knob works
 * When disabled, lookups should always conservatively return true.
 * The value of the knob is cached on first access, so changing the value at runtime should not
 * affect the value PathArrayness sees for a given query.
 */

TEST(ArraynessTrie, LookupTrieWithQueryKnobDisabled) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();

    // Disable query knob
    RAIIServerParameterControllerForTest queryKnobController("internalEnablePathArrayness", false);

    // Array: ["a.b"]
    FieldPath field_AB("a.b");
    MultikeyComponents multikeyPaths_AB{1U};

    // Array: ["a.b"]. Note in this case "a.b" is not an array.
    FieldPath field_ABC("a.b.c");
    MultikeyComponents multikeyPaths_ABC{1U};

    // Note "a" is not an array in field_AB or field_ABC examples above.
    std::string fieldPathString_A = "a";

    // First add field_AB to trie and then add field_ABC to trie.
    std::vector<FieldPath> fields{field_AB, field_ABC};
    std::vector<MultikeyComponents> multikeyness{multikeyPaths_AB, multikeyPaths_ABC};

    PathArrayness pathArrayness;

    pathArrayness.addPath(field_AB, multikeyPaths_AB, true);
    pathArrayness.addPath(field_ABC, multikeyPaths_ABC, true);

    // Path "a" is not an array in either of the paths inserted into the trie, but since the
    // query knob is disabled PathArrayness should conservatively return true.
    // Test for both FieldPath and FieldRef.
    ASSERT_EQ(pathArrayness.isPathArray(FieldPath(fieldPathString_A), &expCtx), true);
    ASSERT_EQ(pathArrayness.isPathArray(FieldRef(fieldPathString_A), &expCtx), true);

    // Enable query knob
    queryKnobController = RAIIServerParameterControllerForTest("internalEnablePathArrayness", true);

    // The original value of the query knob is cached in the ExpressionContext, so even though
    // the query knob has now been enabled PathArrayness will still use the cached value
    // (disabled) and conservatively return true. This ensures consistent results during the
    // lifetime of any given query.
    // Test for both FieldPath and FieldRef.
    ASSERT_EQ(pathArrayness.isPathArray(FieldPath(fieldPathString_A), &expCtx), true);
    ASSERT_EQ(pathArrayness.isPathArray(FieldRef(fieldPathString_A), &expCtx), true);
}

TEST(ArraynessTrie, LookupTrieWithQueryKnobEnabled) {
    ExpressionContextForTest expCtx = ExpressionContextForTest();

    // Disable query knob
    RAIIServerParameterControllerForTest queryKnobController("internalEnablePathArrayness", true);

    // Array: ["a.b"]
    FieldPath field_AB("a.b");
    MultikeyComponents multikeyPaths_AB{1U};

    // Array: ["a.b"]. Note in this case "a.b" is not an array.
    FieldPath field_ABC("a.b.c");
    MultikeyComponents multikeyPaths_ABC{1U};

    // Note "a" is not an array in field_AB or field_ABC examples above.
    std::string fieldPathString_A = "a";

    // First add field_AB to trie and then add field_ABC to trie.
    std::vector<FieldPath> fields{field_AB, field_ABC};
    std::vector<MultikeyComponents> multikeyness{multikeyPaths_AB, multikeyPaths_ABC};

    PathArrayness pathArrayness;

    pathArrayness.addPath(field_AB, multikeyPaths_AB, true);
    pathArrayness.addPath(field_ABC, multikeyPaths_ABC, true);

    // Path "a" is not an array in either of the paths inserted into the trie and the query knob is
    // enabled so we should see that the path is not an array.
    // Test for both FieldPath and FieldRef.
    ASSERT_EQ(pathArrayness.isPathArray(FieldPath(fieldPathString_A), &expCtx), false);
    ASSERT_EQ(pathArrayness.isPathArray(FieldRef(fieldPathString_A), &expCtx), false);

    // Enable query knob
    queryKnobController =
        RAIIServerParameterControllerForTest("internalEnablePathArrayness", false);

    // The original value of the query knob is cached in the ExpressionContext, so even though
    // the query knob has now been disabled PathArrayness will still use the cached value
    // (enabled) and search the trie for arrayness values. This ensures consistent results during
    // the lifetime of any given query.
    // Test for both FieldPath and FieldRef.
    ASSERT_EQ(pathArrayness.isPathArray(FieldPath(fieldPathString_A), &expCtx), false);
    ASSERT_EQ(pathArrayness.isPathArray(FieldRef(fieldPathString_A), &expCtx), false);
}

}  // namespace mongo
