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
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(ArraynessTrie, InsertIntoTrie) {

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

    // Array: ["a"]
    FieldPath field_ABDE("a.b.d.e");
    MultikeyComponents multikeyPaths_ABDE{0U};

    // Array: []
    FieldPath field_BDE("b.d.e");
    MultikeyComponents multikeyPaths_BDE{};

    std::vector<FieldPath> fields{field_A, field_ABC, field_ABD, field_ABCJ, field_ABDE, field_BDE};
    std::vector<MultikeyComponents> multikeyness{multikeyPaths_A,
                                                 multikeyPaths_ABC,
                                                 multikeyPaths_ABD,
                                                 multikeyPaths_ABCJ,
                                                 multikeyPaths_ABDE,
                                                 multikeyPaths_BDE};

    PathArrayness trie;

    for (size_t i = 0; i < fields.size(); i++) {
        trie.addPath(fields[i], multikeyness[i]);
    }

    trie.visualizeTrie();

    ASSERT_EQ(trie.isPathArray(field_A), true);
}

}  // namespace mongo
