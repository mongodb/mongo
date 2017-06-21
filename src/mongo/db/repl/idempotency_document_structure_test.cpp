/**
 * Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/db/repl/idempotency_document_structure.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

std::vector<BSONObj> getEnumeratedDocs(std::set<StringData> fields, size_t depth, size_t length) {
    DocumentStructureEnumerator enumerator(fields, depth, length);
    return enumerator.getDocs();
}

TEST(DocGenTest, NumDocsIsCorrect) {
    std::vector<BSONObj> docs = getEnumeratedDocs({"a", "b"}, 2, 1);
    ASSERT_EQUALS(docs.size(), 104U);
}

TEST(DocGenTest, NoDuplicateDocs) {
    std::vector<BSONObj> docs = getEnumeratedDocs({"a", "b"}, 2, 1);
    for (size_t i = 0; i < docs.size(); i++) {
        for (size_t j = i + 1; j < docs.size(); j++) {
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
    size_t depth = 2;
    size_t length = 1;
    DocumentStructureEnumerator enumerator(fields, depth, length);
    BSONObj start;
    bool docFound = false;
    for (auto doc : enumerator) {
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
    for (size_t i = 0; i < smallerSize; i++) {
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
    // Although I could re-use some of these BSONObj (e.g. doc2 inside doc1), I think maintaining
    // the order demonstrates something about our enumeration path that is valuable.
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

    auto enumeratedDocs = getEnumeratedDocs({"a", "b"}, 2, 0);
    testEnumeratedDocsAreCorrect(expectedDocs, enumeratedDocs);
}

TEST(DocGenTest, EntireCollectionExistsABDepth1Length2) {
    // Although we could re-use some of these BSONObj (e.g. doc2 inside doc1), we think maintaining
    // the order demonstrates something about our enumeration order that is valuable.
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

    auto enumeratedDocs = getEnumeratedDocs({"a", "b"}, 1, 2);
    testEnumeratedDocsAreCorrect(expectedDocs, enumeratedDocs);
}

TEST(EnumerateArrsTest, NumArrsIsCorrect) {
    std::set<StringData> fields{"a"};
    size_t depth = 2;
    size_t length = 2;
    DocumentStructureEnumerator enumerator(fields, depth, length);
    std::vector<BSONArray> arrs = enumerator.enumerateArrs();
    ASSERT_EQUALS(arrs.size(), 2365U);
}

TEST(EnumerateArrsTest, NoDuplicateArrs) {
    std::set<StringData> fields{"a", "b"};
    size_t depth = 2;
    size_t length = 2;
    DocumentStructureEnumerator enumerator(fields, depth, length);
    BSONObj start;
    std::vector<BSONArray> arrs = enumerator.enumerateArrs();
    for (size_t i = 0; i < arrs.size(); i++) {
        for (size_t j = i + 1; j < arrs.size(); j++) {
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
