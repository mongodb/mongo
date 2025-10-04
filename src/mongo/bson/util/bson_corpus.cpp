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

#include "mongo/base/data_view.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/errno_util.h"

#include <algorithm>
#include <climits>
#include <fstream>
#include <string>

#include <boost/filesystem/operations.hpp>
#include <boost/program_options.hpp>
#include <boost/program_options/errors.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/variables_map.hpp>

namespace {
/* Writes a nested tree of sub objects to a BSONObjBuilder.
 * depth - number of levels of nesting in the tree
 * width - branching factor at each level of the tree
 * ob - BSONObjBuilder to receive nested object
 * level - passed recursively to track depth of the tree
 * elementIndex - passed recursively to track index width-wise of element
 */
void addNest(int depth, int width, mongo::BSONObjBuilder& ob, int level = 0, int elementIndex = 0) {
    static const std::string fieldnames[5] = {"1", "2", "3", "4", "5"};

    if (level == depth) {
        ob.append("foo", elementIndex);
        ob.append("bar", (double)elementIndex);
    } else {
        for (int i = 0; i < width; ++i) {
            mongo::BSONObjBuilder subObjBuilder(ob.subobjStart(fieldnames[i]));
            addNest(depth, width, subObjBuilder, level + 1, i);
        }
    }
};

/* Construct a string that fills up remaining available
 * BSONObj size after all metadata and adds it to a BSONObjBuilder.
 */
void fillMaxSize(mongo::BSONObjBuilder& ob) {
    constexpr size_t insertOverhead = 16 * 1024;  // Size added for db insert msg
    const size_t bufSize = mongo::BSONObjMaxInternalSize - ob.len() -
        sizeof(uint8_t) -   // control char
        strlen("string") -  // fieldname
        1 -                 // null terminator
        sizeof(uint32_t) -  // size of string content
        1 -                 // appendStr adds a terminating null
        sizeof(uint8_t) -   // eoo
        insertOverhead;
    auto buf = std::make_unique<char[]>(bufSize);
    memset(buf.get(), 1, bufSize);
    ob.append("string", mongo::StringData(buf.get(), bufSize));
};

// We cannot use a constexpr since BSONObjs and their builders are non-literal types,
// this constructs a read-only const value we can reference, and only builds it once
// regardless of the number of times we call.
//
// Extending the corpus is simply done by adding more calls to genBSONObj();
const std::vector<std::string>& corpus() {
    static const std::vector<std::string> vec = [] {
        std::vector<std::string> v;

        int64_t id = 0;
        auto genBSONObj = [&](const std::function<void(mongo::BSONObjBuilder&)>& gen) {
            mongo::BSONObjBuilder ob;
            mongo::OID newId;
            newId.initFromTermNumber(id);
            ob.appendOID("_id", &newId);
            gen(ob);
            const mongo::BSONObj obj = ob.done();
            v.emplace_back(obj.objdata(), obj.objsize());
            id++;
        };

        // Case 0: Edge-case scalars
        genBSONObj([](mongo::BSONObjBuilder& ob) -> void {
            ob.appendNumber("max int", INT_MAX);
            ob.appendNumber("max long", LLONG_MAX);
            ob.appendNumber("max double", DBL_MAX);
            ob.appendNumber("zero int", 0);
            ob.appendNumber("zero long", 0.0);
            ob.appendNumber("zero double", 0ll);
            ob.appendNumber("min int", INT_MIN);
            ob.appendNumber("min long", LLONG_MIN);
            ob.appendNumber("min double", DBL_MIN);
            ob.appendNumber("max 128-bit positive", mongo::Decimal128::kLargestPositive);
            ob.appendNumber("min 128-bit positive", mongo::Decimal128::kSmallestPositive);
            ob.appendNumber("max 128-bit negative", mongo::Decimal128::kLargestNegative);
            ob.appendNumber("min 128-bit negative", mongo::Decimal128::kSmallestNegative);
            ob.appendNumber("zero 128-bit", mongo::Decimal128::kNormalizedZero);
            ob.appendNumber("NaN positive 128-bit", mongo::Decimal128::kPositiveNaN);
            ob.appendNumber("NaN negative 128-bit", mongo::Decimal128::kNegativeNaN);
            ob.appendNumber("inf positive 128-bit", mongo::Decimal128::kPositiveInfinity);
            ob.appendNumber("inf negative 128-bit", mongo::Decimal128::kNegativeInfinity);
        });

        // Case 1: Max BSON size handleable by server
        genBSONObj([&](mongo::BSONObjBuilder& ob) -> void { fillMaxSize(ob); });

        // Case 2:
        // Sub object with a broad branching factor: each object contains 5.
        // Sub objects which in turn branch into 5 further subobjects, etc.
        genBSONObj([&](mongo::BSONObjBuilder& ob) -> void { addNest(8, 5, ob); });

        // Case 3:
        // Deeply nested object: single nested chain of subobjects up to
        // maximum nesting depth allowed in js representation of bson object, minus levels for
        // containing message.
        constexpr size_t maxJSBsonDepth = 147;
        genBSONObj([&](mongo::BSONObjBuilder& ob) -> void { addNest(maxJSBsonDepth, 1, ob); });

        // Case 4: Creates as many nested objects as can fit in max size allowance.
        genBSONObj([&](mongo::BSONObjBuilder& ob) -> void {
            // Create one subobject and measure how much size was added.
            size_t prevLen = ob.len();
            addNest(6, 3, ob);
            size_t afterLen = ob.len();
            size_t objLen = afterLen - prevLen;

            // Based on previous measurement, calculate how many more subobjects
            // of same size we can fit, this is equal to
            // (maximum size - previously consumed amount - final EOO character) / object size
            // and rounded down.
            size_t fillIters = (mongo::BSONObjMaxInternalSize - afterLen - 1) / objLen;
            for (size_t i = 0; i < fillIters; ++i)
                addNest(6, 3, ob);
        });

        // Case 5: One array with ~million elements.
        genBSONObj([&](mongo::BSONObjBuilder& ob) -> void {
            mongo::UniqueBSONArrayBuilder ab;
            for (int i = 0; i < 1 << 20; ++i) {
                ab.append(i);
            }
            ab.done();
            ob.append("array", ab.arr());
        });

        // Case 6: 1024 arrays each with 5 elements.
        genBSONObj([&](mongo::BSONObjBuilder& ob) -> void {
            for (int i = 0; i < 1024; ++i) {
                // This is used as fieldname for each inserted subarray.
                // Use array counter to make distinct names.
                char buf[256];
                sprintf(buf, "array-%d", i);

                mongo::UniqueBSONArrayBuilder ab;
                for (int j = 0; j < 5; ++j) {
                    ab.append(i * j);
                }
                ab.done();
                ob.append(buf, ab.arr());
            }
        });

        // Case 7: Exercise remaining types and fill size limit.
        genBSONObj([&](mongo::BSONObjBuilder& ob) -> void {
            mongo::BSONObjBuilder subObject;
            subObject.append("a", 1);
            subObject.done();

            char binaryContent[1024];
            memset(binaryContent, 1, 1024);

            unsigned char oidContent[12];
            memset(oidContent, 1, 12);
            mongo::OID oid(oidContent);
            long long millis = 0;

            ob.appendMinKey("minkey");
            ob.appendMaxKey("maxkey");
            ob.appendBinData("bindata", 1024, mongo::BinDataType::BinDataGeneral, binaryContent);
            ob.appendOID("oid", &oid);
            ob.appendBool("bool", true);
            ob.appendDate("date", mongo::Date_t::fromMillisSinceEpoch(millis));
            ob.appendNull("null");
            ob.appendRegex("regex", mongo::StringData(binaryContent, 1024));
            ob.appendDBRef("dbref", mongo::StringData(binaryContent, 1024), oid);
            ob.appendCode("code", mongo::StringData(binaryContent, 1024));
            ob.appendSymbol("symbol", mongo::StringData(binaryContent, 1024));
            ob.appendCodeWScope(
                "codewscope", mongo::StringData(binaryContent, 1024), subObject.obj());
            ob.appendTimestamp("timestamp", 1ll);

            fillMaxSize(ob);
        });

        // Case 8: Add a bunch of invalid null bytes in the middle of a string
        genBSONObj([&](mongo::BSONObjBuilder& ob) -> void {
            const std::array<mongo::StringData, 9> bunchOfStringsWithNulls = {
                // Starts with null chars.
                mongo::StringData{"\x00a", 2},
                // Ends with null chars.
                mongo::StringData{"a\x00", 2},
                // All null chars.
                mongo::StringData{"\x00", 1},
                mongo::StringData{"\x00\x00\x00", 3},
                // Null chars somewhere in the middle.
                mongo::StringData{"a\x00\x01\x08a", 5},
                mongo::StringData{"a\x00\x02\x08b", 5},
                mongo::StringData{"a\x00\x01\x10", 4},
                mongo::StringData{"a\x00\x01\xc0", 4},
                mongo::StringData{"a\x00\x01\x03d\x00\xff\xff\xff\xff\x00\x08b", 13}};
            std::string name = "String";
            for (size_t i = 0; i < bunchOfStringsWithNulls.size(); ++i)
                ob.append(name + std::to_string(i), bunchOfStringsWithNulls[i]);
        });

        return v;
    }();
    return vec;
}

}  // namespace

namespace mongo {
namespace bson {

/**
 * This library generates a corpus of interesting BSON objects intended to be used
 * for testing purposes.  They are accessed an individual object at a time so as to be
 * usable in jstests without overflowing a BSONArray (individual objects in this corpus
 * hit the BSONObject size limit).
 */

/**
 * Returns the number of objects in the corpus.
 */
int corpusSize() {
    return static_cast<int>(corpus().size());  // Cast to int to simplify bson conversions
}

/**
 * Returns a corpus object by index
 */
const std::string& getCorpusObject(int index) {
    uassert(9479200, "Index exceeds corpus limits", index < corpusSize());

    return corpus().at(index);
}

}  // namespace bson
}  // namespace mongo
