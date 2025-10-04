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

// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/db/repl/idempotency_document_structure.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cstddef>
#include <memory>

namespace mongo {
namespace {

std::vector<BSONObj> getEnumeratedDocs(DocumentStructureEnumeratorConfig config) {
    TrivialScalarGenerator trivialScalarGenerator;
    DocumentStructureEnumerator enumerator(config, &trivialScalarGenerator);
    return enumerator.getDocs();
}

TEST(DocGenTest, NumDocsIsCorrect) {
    std::vector<BSONObj> docs = getEnumeratedDocs({{"a", "b"}, 2, 1, false, false});
    ASSERT_EQUALS(docs.size(), 104U);
    docs = getEnumeratedDocs({{"a", "b"}, 2, 1, true, false});
    ASSERT_EQUALS(docs.size(), 36U);
    docs = getEnumeratedDocs({{"a", "b"}, 2, 1, false, true});
    ASSERT_EQUALS(docs.size(), 15U);
    docs = getEnumeratedDocs({{"a", "b"}, 2, 1, true, true});
    ASSERT_EQUALS(docs.size(), 4U);
}

TEST(DocGenTest, NoDuplicateDocs) {
    std::vector<BSONObj> docs = getEnumeratedDocs({{"a", "b"}, 2, 1});
    for (std::size_t i = 0; i < docs.size(); i++) {
        for (std::size_t j = i + 1; j < docs.size(); j++) {
            if (docs[i].binaryEqual(docs[j])) {
                StringBuilder sb;
                sb << "outer doc: " << docs[i] << " matches with inner: " << docs[j];
                FAIL(sb.str());
            }
        }
    }
}

TEST(DocGenTest, SomePreChosenDocExists) {
    BSONObj specialDoc = BSON("a" << BSON("b" << 0));

    std::set<StringData> fields{"a", "b"};
    std::size_t depth = 2;
    std::size_t length = 1;
    TrivialScalarGenerator trivialScalarGenerator;
    DocumentStructureEnumerator enumerator({fields, depth, length}, &trivialScalarGenerator);
    BSONObj start;
    bool docFound = false;
    for (const auto& doc : enumerator) {
        if (doc.binaryEqual(specialDoc)) {
            docFound = true;
            break;
        }
    }

    if (!docFound) {
        StringBuilder sb;
        sb << "Did not find " << specialDoc;
        FAIL(sb.str());
    }
}

void testEnumeratedDocsAreCorrect(const std::vector<BSONObj>& enumeratedDocs,
                                  const std::vector<BSONObj>& expectedDocs) {
    auto smallerSize = std::min(enumeratedDocs.size(), expectedDocs.size());
    for (std::size_t i = 0; i < smallerSize; i++) {
        if (!expectedDocs[i].binaryEqual(enumeratedDocs[i])) {
            StringBuilder sb;
            sb << "Expected to find " << expectedDocs[i] << " but found " << enumeratedDocs[i];
            FAIL(sb.str());
        }
    }

    StringBuilder sb;
    if (expectedDocs.size() > enumeratedDocs.size()) {
        sb << "Expected to find ";
        sb << expectedDocs.back();
        sb << " but didn't";
        FAIL(sb.str());
    } else if (expectedDocs.size() < enumeratedDocs.size()) {
        sb << "Unexpectedly found ";
        sb << enumeratedDocs.back();
        FAIL(sb.str());
    }
}

TEST(DocGenTest, EntireCollectionExistsABDepth2Length0) {
    std::vector<BSONObj> expectedDocs;
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : 0 }"));
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : []}"));
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : {}}"));
    expectedDocs.push_back(fromjson("{'b' : 0}"));
    expectedDocs.push_back(fromjson("{}"));
    expectedDocs.push_back(fromjson("{'b' : [] }"));
    expectedDocs.push_back(fromjson("{'b' : {} }"));
    expectedDocs.push_back(fromjson("{'a' : [], 'b' : 0 }"));
    expectedDocs.push_back(fromjson("{'a' : []}"));
    expectedDocs.push_back(fromjson("{'a' : [], 'b' : []}"));
    expectedDocs.push_back(fromjson("{'a' : [], 'b' : {}}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : 0}, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : 0}}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : 0}, 'b' : []}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : 0}, 'b' : {}}"));
    expectedDocs.push_back(fromjson("{'a' : {}, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : {}}"));
    expectedDocs.push_back(fromjson("{'a' : {}, 'b' : []}"));
    expectedDocs.push_back(fromjson("{'a' : {}, 'b' : {}}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : []}, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : []}}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : []}, 'b' : []}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : []}, 'b' : {}}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : {}}, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : {}}}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : {}}, 'b' : []}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : {}}, 'b' : {}}"));

    auto enumeratedDocs = getEnumeratedDocs({{"a", "b"}, 2, 0});
    testEnumeratedDocsAreCorrect(enumeratedDocs, expectedDocs);
}

TEST(DocGenTest, EntireCollectionExistsABDepth2Length0ArrsDisabled) {
    std::vector<BSONObj> expectedDocs;
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : 0 }"));
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : {}}"));
    expectedDocs.push_back(fromjson("{'b' : 0}"));
    expectedDocs.push_back(fromjson("{}"));
    expectedDocs.push_back(fromjson("{'b' : {} }"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : 0}, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : 0}}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : 0}, 'b' : {}}"));
    expectedDocs.push_back(fromjson("{'a' : {}, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : {}}"));
    expectedDocs.push_back(fromjson("{'a' : {}, 'b' : {}}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : {}}, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : {}}}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : {}}, 'b' : {}}"));

    TrivialScalarGenerator trivialScalarGenerator;
    DocumentStructureEnumerator enumerator({{"a", "b"}, 2, 0, false, true},
                                           &trivialScalarGenerator);
    testEnumeratedDocsAreCorrect(enumerator.getDocs(), expectedDocs);
}

TEST(DocGenTest, EntireCollectionExistsABDepth2Length0DocsDisabled) {
    std::vector<BSONObj> expectedDocs;
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : 0 }"));
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : []}"));
    expectedDocs.push_back(fromjson("{'b' : 0}"));
    expectedDocs.push_back(fromjson("{}"));
    expectedDocs.push_back(fromjson("{'b' : [] }"));
    expectedDocs.push_back(fromjson("{'a' : [], 'b' : 0 }"));
    expectedDocs.push_back(fromjson("{'a' : []}"));
    expectedDocs.push_back(fromjson("{'a' : [], 'b' : []}"));

    TrivialScalarGenerator trivialScalarGenerator;
    DocumentStructureEnumerator enumerator({{"a", "b"}, 2, 0, true, false},
                                           &trivialScalarGenerator);
    testEnumeratedDocsAreCorrect(enumerator.getDocs(), expectedDocs);
}

TEST(DocGenTest, EntireCollectionExistsABDepth2Length0BothDisabled) {
    std::vector<BSONObj> expectedDocs;
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : 0 }"));
    expectedDocs.push_back(fromjson("{'b' : 0}"));
    expectedDocs.push_back(fromjson("{}"));

    TrivialScalarGenerator trivialScalarGenerator;
    DocumentStructureEnumerator enumerator({{"a", "b"}, 2, 0, true, true}, &trivialScalarGenerator);
    testEnumeratedDocsAreCorrect(enumerator.getDocs(), expectedDocs);
}

TEST(DocGenTest, EntireCollectionExistsABDepth1Length2) {
    std::vector<BSONObj> expectedDocs;
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : []}"));
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : [0]}"));
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : [0, 0]}"));
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : {}}"));
    expectedDocs.push_back(fromjson("{'b' : 0}"));
    expectedDocs.push_back(fromjson("{}"));
    expectedDocs.push_back(fromjson("{'b' : []}"));
    expectedDocs.push_back(fromjson("{'b' : [0]}"));
    expectedDocs.push_back(fromjson("{'b' : [0, 0]}"));
    expectedDocs.push_back(fromjson("{'b' : {}}"));
    expectedDocs.push_back(fromjson("{'a' : [], 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : []}"));
    expectedDocs.push_back(fromjson("{'a' : [], 'b' : []}"));
    expectedDocs.push_back(fromjson("{'a' : [], 'b' : [0]}"));
    expectedDocs.push_back(fromjson("{'a' : [], 'b' : [0, 0]}"));
    expectedDocs.push_back(fromjson("{'a' : [], 'b' : {}}"));
    expectedDocs.push_back(fromjson("{'a' : [0], 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : [0]}"));
    expectedDocs.push_back(fromjson("{'a' : [0], 'b' : []}"));
    expectedDocs.push_back(fromjson("{'a' : [0], 'b' : [0]}"));
    expectedDocs.push_back(fromjson("{'a' : [0], 'b' : [0, 0]}"));
    expectedDocs.push_back(fromjson("{'a' : [0], 'b' : {}}"));
    expectedDocs.push_back(fromjson("{'a' : [0, 0], 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : [0, 0]}"));
    expectedDocs.push_back(fromjson("{'a' : [0, 0], 'b' : []}"));
    expectedDocs.push_back(fromjson("{'a' : [0, 0], 'b' : [0]}"));
    expectedDocs.push_back(fromjson("{'a' : [0, 0], 'b' : [0, 0]}"));
    expectedDocs.push_back(fromjson("{'a' : [0, 0], 'b' : {}}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : 0}, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : 0}}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : 0}, 'b' : []}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : 0}, 'b' : [0]}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : 0}, 'b' : [0, 0]}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : 0}, 'b' : {}}"));
    expectedDocs.push_back(fromjson("{'a' : {}, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : {}}"));
    expectedDocs.push_back(fromjson("{'a' : {}, 'b' : []}"));
    expectedDocs.push_back(fromjson("{'a' : {}, 'b' : [0]}"));
    expectedDocs.push_back(fromjson("{'a' : {}, 'b' : [0, 0]}"));
    expectedDocs.push_back(fromjson("{'a' : {}, 'b' : {}}"));

    auto enumeratedDocs = getEnumeratedDocs({{"a", "b"}, 1, 2});
    testEnumeratedDocsAreCorrect(enumeratedDocs, expectedDocs);
}

TEST(DocGenTest, EntireCollectionExistsABDepth1Length2ArrsDisabled) {
    std::vector<BSONObj> expectedDocs;
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : {}}"));
    expectedDocs.push_back(fromjson("{'b' : 0}"));
    expectedDocs.push_back(fromjson("{}"));
    expectedDocs.push_back(fromjson("{'b' : {}}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : 0}, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : 0}}"));
    expectedDocs.push_back(fromjson("{'a' : {'b' : 0}, 'b' : {}}"));
    expectedDocs.push_back(fromjson("{'a' : {}, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : {}}"));
    expectedDocs.push_back(fromjson("{'a' : {}, 'b' : {}}"));

    TrivialScalarGenerator trivialScalarGenerator;
    DocumentStructureEnumerator enumerator({{"a", "b"}, 1, 2, false, true},
                                           &trivialScalarGenerator);
    testEnumeratedDocsAreCorrect(enumerator.getDocs(), expectedDocs);
}

TEST(DocGenTest, EntireCollectionExistsABDepth1Length2DocsDisabled) {
    std::vector<BSONObj> expectedDocs;
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : []}"));
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : [0]}"));
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : [0, 0]}"));
    expectedDocs.push_back(fromjson("{'b' : 0}"));
    expectedDocs.push_back(fromjson("{}"));
    expectedDocs.push_back(fromjson("{'b' : []}"));
    expectedDocs.push_back(fromjson("{'b' : [0]}"));
    expectedDocs.push_back(fromjson("{'b' : [0, 0]}"));
    expectedDocs.push_back(fromjson("{'a' : [], 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : []}"));
    expectedDocs.push_back(fromjson("{'a' : [], 'b' : []}"));
    expectedDocs.push_back(fromjson("{'a' : [], 'b' : [0]}"));
    expectedDocs.push_back(fromjson("{'a' : [], 'b' : [0, 0]}"));
    expectedDocs.push_back(fromjson("{'a' : [0], 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : [0]}"));
    expectedDocs.push_back(fromjson("{'a' : [0], 'b' : []}"));
    expectedDocs.push_back(fromjson("{'a' : [0], 'b' : [0]}"));
    expectedDocs.push_back(fromjson("{'a' : [0], 'b' : [0, 0]}"));
    expectedDocs.push_back(fromjson("{'a' : [0, 0], 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : [0, 0]}"));
    expectedDocs.push_back(fromjson("{'a' : [0, 0], 'b' : []}"));
    expectedDocs.push_back(fromjson("{'a' : [0, 0], 'b' : [0]}"));
    expectedDocs.push_back(fromjson("{'a' : [0, 0], 'b' : [0, 0]}"));

    TrivialScalarGenerator trivialScalarGenerator;
    DocumentStructureEnumerator enumerator({{"a", "b"}, 1, 2, true, false},
                                           &trivialScalarGenerator);
    testEnumeratedDocsAreCorrect(enumerator.getDocs(), expectedDocs);
}

TEST(DocGenTest, EntireCollectionExistsABDepth1Length2BothDisabled) {
    std::vector<BSONObj> expectedDocs;
    expectedDocs.push_back(fromjson("{'a' : 0, 'b' : 0}"));
    expectedDocs.push_back(fromjson("{'a' : 0}"));
    expectedDocs.push_back(fromjson("{'b' : 0}"));
    expectedDocs.push_back(fromjson("{}"));

    TrivialScalarGenerator trivialScalarGenerator;
    DocumentStructureEnumerator enumerator({{"a", "b"}, 1, 2, true, true}, &trivialScalarGenerator);
    testEnumeratedDocsAreCorrect(enumerator.getDocs(), expectedDocs);
}

TEST(EnumerateArrsTest, NumArrsIsCorrect) {
    std::set<StringData> fields{"a"};
    std::size_t depth = 2;
    std::size_t length = 2;
    TrivialScalarGenerator trivialScalarGenerator;
    DocumentStructureEnumerator enumerator({fields, depth, length}, &trivialScalarGenerator);
    std::vector<BSONArray> arrs = enumerator.enumerateArrs();
    ASSERT_EQUALS(arrs.size(), 2414U);
}

TEST(EnumerateArrsTest, NoDuplicateArrs) {
    std::set<StringData> fields{"a", "b"};
    std::size_t depth = 2;
    std::size_t length = 2;
    TrivialScalarGenerator trivialScalarGenerator;
    DocumentStructureEnumerator enumerator({fields, depth, length}, &trivialScalarGenerator);
    BSONObj start;
    std::vector<BSONArray> arrs = enumerator.enumerateArrs();
    for (std::size_t i = 0; i < arrs.size(); i++) {
        for (std::size_t j = i + 1; j < arrs.size(); j++) {
            if (arrs[i].binaryEqual(arrs[j])) {
                StringBuilder sb;
                sb << "outer arr: " << arrs[i] << " matches with inner: " << arrs[j];
                FAIL(sb.str());
            }
        }
    }
}

}  // namespace
}  // namespace mongo
