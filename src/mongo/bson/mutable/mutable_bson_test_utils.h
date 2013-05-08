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

#include <iosfwd>

namespace mongo {

    class BSONObj;

namespace mutablebson {

    class ConstElement;
    class Document;

    //
    // Utilities for mutable BSON unit tests.
    //

    /**
     * Catch all comparator between a mutable 'doc' and the expected BSON 'exp'. It compares
     * (a) 'doc's generated object, (b) 'exp', the expected object, and (c) 'doc(exp)', a
     * document created from 'exp'. Returns true if all three are equal, otherwise false.
     */
    bool checkDoc(const Document& doc, const BSONObj& exp);

    /** Stream out an element; useful within ASSERT calls */
    std::ostream& operator<<(std::ostream& stream, const ConstElement& elt);

} // namespace mutablebson
} // namespace mongo
