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
#include <boost/filesystem/operations.hpp>
#include <boost/program_options.hpp>
#include <boost/program_options/errors.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/variables_map.hpp>
#include <climits>
#include <fstream>
#include <string>

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
    const size_t bufSize = mongo::BSONObjMaxInternalSize - ob.len() -
        sizeof(uint8_t) -   // control char
        strlen("string") -  // fieldname
        1 -                 // null terminator
        sizeof(uint32_t) -  // size of string content
        1 -                 // appendStr adds a terminating null
        sizeof(uint8_t);    // eoo
    auto buf = std::make_unique<char[]>(bufSize);
    memset(buf.get(), 1, bufSize);
    ob.append("string", mongo::StringData(buf.get(), bufSize));
};
}  // namespace

/*
 * Writes a corpus of bson edge cases to stdout, or to file specified by --output.
 *
 * We could have just provided a const corpus (possibly in json to be converted),
 * however it is likely we will have some edge cases that are easier to specify
 * programmatically. It is also likely such cases will be easier to read and interpret
 * if specified in code rather than a large constant expr. So in the interest of
 * keeping things simple for future extensions of the corpus, we will instead create a
 * nunber of case generators in this command line utility.
 */

int main(int argc, char* argv[]) {

    mongo::Status status =
        mongo::runGlobalInitializers(std::vector<std::string>(argv, argv + argc));
    if (!status.isOK()) {
        std::cerr << "Failed global initialization: " << status << std::endl;
        return static_cast<int>(mongo::ExitCode::fail);
    }

    boost::program_options::variables_map vm;
    std::ofstream outputStream;
    try {
        auto outputStr =
            "Path to file output (defaults to 'corpus.bson', will be created if non-existent)";
        boost::program_options::options_description desc{"Options"};
        desc.add_options()("help,h", "help")(
            "output,o",
            boost::program_options::value<std::string>()->default_value("corpus.bson"),
            outputStr);

        store(parse_command_line(argc, argv, desc), vm);
        notify(vm);

        if (vm.count("help")) {
            std::cout << "Usage: bson_corpus_gen [options]\n";
            std::cout << "A tool for generating a corpus of BSON documents useful for testing.\n\n";
            std::cout << desc;
            return 0;
        }

        auto outputFile = vm["output"].as<std::string>();
        outputStream.open(outputFile, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!outputStream.is_open()) {
            std::cerr << "Error writing to file: " << outputFile << std::endl;
            return static_cast<int>(mongo::ExitCode::fail);
        }
    } catch (const boost::program_options::error& ex) {
        std::cerr << ex.what() << '\n';
        return static_cast<int>(mongo::ExitCode::fail);
    }

    auto genBSONObj = [&](const std::function<void(mongo::BSONObjBuilder&)>& gen) {
        mongo::BSONObjBuilder ob;
        gen(ob);
        const mongo::BSONObj obj = ob.done();
        outputStream.write(obj.objdata(), obj.objsize());
    };

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

    genBSONObj([&](mongo::BSONObjBuilder& ob) -> void { fillMaxSize(ob); });

    // Sub object with a broad branching factor: each object contains 5.
    // Sub objects which in turn branch into 5 further subobjects, etc.
    genBSONObj([&](mongo::BSONObjBuilder& ob) -> void { addNest(8, 5, ob); });

    // Deeply nested object: single nested chain of subobjects up to
    // maximum bson nesting depth.
    genBSONObj([&](mongo::BSONObjBuilder& ob) -> void {
        addNest(mongo::BSONDepth::getMaxAllowableDepth(), 1, ob);
    });

    // Creates as many nested objects as can fit in max size allowance.
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

    // One array with ~million elements.
    genBSONObj([&](mongo::BSONObjBuilder& ob) -> void {
        mongo::UniqueBSONArrayBuilder ab;
        for (int i = 0; i < 1 << 20; ++i) {
            ab.append(i);
        }
        ab.done();
        ob.append("array", ab.arr());
    });

    // 1024 arrays each with 5 elements.
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

    // Exercise remaining types and fill size limit.
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
        ob.appendCodeWScope("codewscope", mongo::StringData(binaryContent, 1024), subObject.obj());
        ob.appendTimestamp("timestamp", 0ll);

        fillMaxSize(ob);
    });

    // add a bunch of invalid null bytes in the middle of a string
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
    return 0;
}
