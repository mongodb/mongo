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

    bool checkDoc(const Document& doc, const BSONObj& exp) {
        return doc.getObject().woCompare(exp) == 0;

#if 0
        // Get the fundamental result via mutables woCompare path.
        const bool primary_result = (doc.compareWithBSONObj(exp) == 0);

        // Validate primary result via other comparison paths.
        BSONObj fromDoc = doc.getObject();

        // Check that mutables serialized result matches against its origin.
        ASSERT_EQUALS(primary_result, doc.compareWithBSONObj(fromDoc) == 0);

        // Check that the serialized doc compares against the expected value.
        ASSERT_EQUALS(primary_result, fromDoc.woCompare(exp) == 0);

        return primary_result;
#endif
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
