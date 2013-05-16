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

    // TODO: We should really update this to be an ASSERT_ something, so that we can print out
    // the expected and actual documents.
    bool checkDoc(const Document& doc, const BSONObj& exp) {

        // Get the fundamental result via BSONObj's woCompare path. This is the best starting
        // point, because we think that Document::getObject and the serialization mechanism is
        // pretty well sorted.
        BSONObj fromDoc = doc.getObject();
        const int primaryResult = fromDoc.woCompare(exp);

        // Validate primary result via other comparison paths.
        const int secondaryResult = doc.compareWithBSONObj(exp);

        if (primaryResult == 0) {
            ASSERT_EQUALS(secondaryResult, 0);
        } else if (primaryResult < 0) {
            ASSERT_LESS_THAN(secondaryResult, 0);
        } else {
            ASSERT_GREATER_THAN(secondaryResult, 0);
        }

        // Check that mutables serialized result matches against its origin.
        ASSERT_EQUALS(0, doc.compareWithBSONObj(fromDoc));

        return (primaryResult == 0);
    }

    std::ostream& operator<<(std::ostream& stream, const ConstElement& elt) {
        stream << "Element("<< static_cast<const void*>(&elt.getDocument()) << ", ";
        if (elt.ok())
            stream << elt.getIdx();
        else
            stream << "kInvalidIndex";
        stream << ")";

        if (elt.ok()) {
            stream << " of type '" << typeName(elt.getType()) << "'";
            if (elt.hasValue())
                stream << " with accessible value '" << elt.getValue() << "'";
            else
                stream << " with no accessible value";
        }

        return stream;
    }

} // namespace mutablebson
} // namespace mongo
