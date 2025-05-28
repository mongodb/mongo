/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/bson/column/bsoncolumn_fuzzer_util.h"
#include "mongo/bson/column/bsoncolumnbuilder.h"
#include "mongo/bson/column/simple8b_helpers.h"
#include "mongo/bson/column/simple8b_type_util.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/base64.h"

#include <cstring>

using namespace mongo;

/**
 * Check that the BSONElement sequence passed to BSONColumnBuilder does not
 * fatal, and that the result decodes to the original sequence we passed.
 */
extern "C" int LLVMFuzzerTestOneInput(const char* Data, size_t Size) {
    using namespace mongo;
    std::forward_list<BSONObj> elementMemory;
    std::vector<BSONElement> generatedElements;

    // Generate elements from input data
    const char* ptr = Data;
    const char* end = Data + Size;
    size_t totalSize = 0;
    while (ptr < end) {
        BSONElement element;
        int repetition;
        if (!mongo::bsoncolumn::createFuzzedElement(ptr, end, elementMemory, repetition, element))
            return 0;  // Bad input string to element generation
        int maxElementSize = (mongo::BSONObjMaxInternalSize + (1 << 10) - totalSize) / repetition;
        if (element.size() > maxElementSize) {
            // We want to allow the inputs to exceed max obj size, but it's not worth
            // testing overly far ahead since our run generation can exceed the
            // fuzzer memory limit if left unchecked.
            return 0;
        }
        if (!bsoncolumn::addFuzzedElements(
                ptr, end, elementMemory, element, repetition, generatedElements)) {
            return 0;  // Bad input string to run generation
        }
        totalSize += repetition * element.size();
    }

    // Exercise the builder
    BSONColumnBuilder builder;
    for (auto element : generatedElements) {
        builder.append(element);
    }

    // Verify decoding gives us original elements
    auto diff = builder.intermediate();
    BSONColumn col(diff.data(), diff.size());
    auto it = col.begin();
    for (auto elem : generatedElements) {
        BSONElement other = *it;
        invariant(elem.binaryEqualValues(other),
                  str::stream() << "Decoded element: '" << it->toString()
                                << "' does not match original: '" << elem.toString()
                                << "'. Column: " << base64::encode(diff.data(), diff.size()));
        invariant(it.more(),
                  str::stream() << "There were fewer decoded elements than original. Column: "
                                << base64::encode(diff.data(), diff.size()));
        ++it;
    }
    invariant(!it.more(),
              str::stream() << "There were more decoded elements than original. Column: "
                            << base64::encode(diff.data(), diff.size()));

    // Verify binary reopen gives identical state as intermediate
    // TODO SERVER-100659: Uncomment this after reopen bug is fixed
    /*
    BSONColumnBuilder reopen(diff.data(), diff.size());
    invariant(builder.isInternalStateIdentical(reopen),
              str::stream() << "Binary reopen does not yield equivalent state. Column: "
                            << base64::encode(diff.data(), diff.size()));
    */

    return 0;
}
