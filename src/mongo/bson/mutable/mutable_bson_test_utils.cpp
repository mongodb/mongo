/* Copyright 2013 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/mutable/mutable_bson_test_utils.h"

#include <ostream>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/mutable/const_element.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace mutablebson {

    namespace {

        inline void assertSameSign(int lhs, int rhs) {
            if (lhs == 0) {
                ASSERT_EQUALS(rhs, 0);
            } else if (lhs < 0) {
                ASSERT_LESS_THAN(rhs, 0);
            } else {
                ASSERT_GREATER_THAN(rhs, 0);
            }
        }

        inline void assertOppositeSign(int lhs, int rhs) {
            if (lhs == 0) {
                ASSERT_EQUALS(rhs, 0);
            } else if (lhs < 0) {
                ASSERT_GREATER_THAN(rhs, 0);
            } else {
                ASSERT_LESS_THAN(rhs, 0);
            }
        }

    } // namespace

    // TODO: We should really update this to be an ASSERT_ something, so that we can print out
    // the expected and actual documents.
    bool checkDoc(const Document& lhs, const BSONObj& rhs) {

        // Get the fundamental result via BSONObj's woCompare path. This is the best starting
        // point, because we think that Document::getObject and the serialization mechanism is
        // pretty well sorted.
        BSONObj fromLhs = lhs.getObject();
        const int primaryResult = fromLhs.woCompare(rhs);

        // Validate primary result via other comparison paths.
        const int secondaryResult = lhs.compareWithBSONObj(rhs);

        assertSameSign(primaryResult, secondaryResult);

        // Check that mutables serialized result matches against its origin.
        ASSERT_EQUALS(0, lhs.compareWithBSONObj(fromLhs));

        return (primaryResult == 0);
    }

    bool checkDoc(const Document& lhs, const Document& rhs) {

        const int primaryResult = lhs.compareWith(rhs);

        const BSONObj fromLhs = lhs.getObject();
        const BSONObj fromRhs = rhs.getObject();

        const int result_d_o = lhs.compareWithBSONObj(fromRhs);
        const int result_o_d = rhs.compareWithBSONObj(fromLhs);

        assertSameSign(primaryResult, result_d_o);
        assertOppositeSign(primaryResult, result_o_d);

        ASSERT_EQUALS(0, lhs.compareWithBSONObj(fromLhs));
        ASSERT_EQUALS(0, rhs.compareWithBSONObj(fromRhs));

        return (primaryResult == 0);
    }

    std::ostream& operator<<(std::ostream& stream, const Document& doc) {
        stream << doc.toString();
        return stream;
    }

    std::ostream& operator<<(std::ostream& stream, const ConstElement& elt) {
        stream << elt.toString();
        return stream;
    }

} // namespace mutablebson
} // namespace mongo
