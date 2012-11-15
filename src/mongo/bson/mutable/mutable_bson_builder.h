/* Copyright 2010 10gen Inc.
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

#pragma once

#include "mongo/bson/mutable/mutable_bson.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace mutablebson {

    /** static method for creating a MutableBSON tree from a BSONObj. */
    class ElementBuilder {
    public:
        static Status parse(const BSONObj& src, Element* dst);

        static Status parse(const BSONObj& src, Document* dst) {
            return parse(src, &dst->root());
        }
    };

    /** static method for creating BSONObj from MutableBSON */
    class BSONBuilder {
    public:
        static void buildFromElement(Element src, BSONObjBuilder* dst);
        static void build(Element src, BSONObjBuilder* dst);

        static void build(const Document& src, BSONObjBuilder* dst) {
            return build(src.root(), dst);
        }

    };

} // namespace mutablebson
} // namespace mongo
