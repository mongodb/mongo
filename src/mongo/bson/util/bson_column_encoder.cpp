/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/bson/util/bsoncolumnbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/errno_util.h"
#include <algorithm>
#include <boost/filesystem/operations.hpp>
#include <boost/program_options.hpp>
#include <boost/program_options/errors.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/variables_map.hpp>
#include <fstream>
#include <string>

using namespace mongo;
using namespace std;

namespace {

/* Finalize a BSONColumn and write it to an ofstream, pre-pended with length of encoding */
void finishBSONColumn(BSONColumnBuilder<>& columnBuilder, char* lenBuf, ofstream& outputStream) {
    BSONBinData binData = columnBuilder.finalize();
    DataView(lenBuf).write<LittleEndian<uint32_t>>(binData.length);
    outputStream.write(lenBuf, sizeof(uint32_t));
    outputStream.write((const char*)binData.data, binData.length);
}

/* Read a BSONColumn from ifstream into buf, return 0 on stream end, -1 on failure */
int readBSONColumn(ifstream& inputStream, char* lenBuf, char* buf) {
    inputStream.read(lenBuf, sizeof(uint32_t));
    if (inputStream.gcount() == 0)
        return 0;
    if (inputStream.gcount() < (streamsize)sizeof(uint32_t)) {
        cerr << "Encountered incomplete BSONColumn length" << endl;
        return -1;
    }
    auto len = ConstDataView(lenBuf).read<LittleEndian<uint32_t>>();
    inputStream.read(buf, len);
    if (inputStream.gcount() < (streamsize)len) {
        cerr << "Encountered incomplete BSONColumn" << endl;
        return -1;
    }
    return len;
}

}  // namespace

int main(int argc, char* argv[]) {

    Status status = mongo::runGlobalInitializers(vector<string>(argv, argv + argc));
    if (!status.isOK()) {
        cerr << "Failed global initialization: " << status << endl;
        return static_cast<int>(ExitCode::fail);
    }

    // Handle program options
    boost::program_options::variables_map vm;

    // Command line options
    // input / output files for the reader (input defaults to stdin)
    // Size of buffer, filling this with BSONObj results in a dump out to BSONColumn
    ifstream inputStream;
    ofstream outputStream;
    uint32_t bufferSize = 0;
    bool decode = false;
    bool csv = false;

    try {
        // Define the program options
        auto inputStr = "Path to file input file (defaults to stdin)";
        auto outputStr = "Path to file output (defaults to stdout)";
        auto bufferStr =
            "Size of BSON buffer, once this is exceeded objects are dumped to BSONColumn";
        auto decodeStr = "Decode from BSONColumn";
        auto csvStr =
            "Treat input as comma-separated list of scalars (decimal implies floating point)";
        boost::program_options::options_description desc{"Options"};
        desc.add_options()("help,h",
                           "help")("input,i", boost::program_options::value<string>(), inputStr)(
            "output,o", boost::program_options::value<string>(), outputStr)(
            "bufferSize,b", boost::program_options::value<uint32_t>(), bufferStr)(
            "decode,d", boost::program_options::bool_switch(), decodeStr)(
            "csv,c", boost::program_options::bool_switch(), csvStr);

        // Parse the program options
        store(parse_command_line(argc, argv, desc), vm);
        notify(vm);

        // User can specify an --input param and it must point to a valid file
        if (vm.count("input")) {
            auto inputFile = vm["input"].as<string>();
            if (!boost::filesystem::exists(inputFile.c_str())) {
                cout << "Error: Specified file does not exist (" << inputFile.c_str() << ")"
                     << endl;
                return static_cast<int>(ExitCode::fail);
            }

            // Open the connection to the input file
            inputStream.open(inputFile, ios::in | ios::binary);
            if (!inputStream.is_open()) {
                cerr << "Error opening file: " << strerror(errno) << endl;
                return static_cast<int>(ExitCode::fail);
            }
        }

        // User can specify an --output param and it does not need to point to a valid file
        if (vm.count("output")) {
            auto outputFile = vm["output"].as<string>();

            // Open the connection to the output file
            outputStream.open(outputFile, ios::out | ios::trunc | ios::binary);
            if (!outputStream.is_open()) {
                cerr << "Error writing to file: " << outputFile << endl;
                return static_cast<int>(ExitCode::fail);
            }
        } else {
            // output to cout
            outputStream.copyfmt(cout);
            outputStream.clear(cout.rdstate());
            outputStream.basic_ios<char>::rdbuf(cout.rdbuf());
        }

        // User can specify a --bufferSize param
        if (vm.count("bufferSize")) {
            bufferSize = vm["bufferSize"].as<uint32_t>();
        }

        // User can specify --decode to decompress BSONColumn data
        if (vm.count("decode")) {
            decode = vm["decode"].as<bool>();
        }

        // User can specify --csv to handle CSV data
        if (vm.count("csv")) {
            csv = vm["csv"].as<bool>();
        }
    } catch (const boost::program_options::error& ex) {
        cerr << ex.what() << '\n';
        return static_cast<int>(ExitCode::fail);
    }

    auto buf = make_unique<char[]>(BSONObjMaxInternalSize);
    char lenBuf[sizeof(uint32_t)];
    if (!decode) {
        size_t encoded = 0;
        BSONColumnBuilder columnBuilder;
        if (!csv) {
            // compressing sequence of BSONObj
            memset(lenBuf, 0, sizeof(uint32_t));
            outputStream.write(lenBuf,
                               sizeof(uint32_t));  // 0 for field count indicates obj sequence
            while (true) {
                inputStream.read(buf.get(), sizeof(uint32_t));
                if (inputStream.gcount() == 0)
                    break;
                if (inputStream.gcount() < (streamsize)sizeof(uint32_t)) {
                    cerr << "Encountered incomplete BSON object length" << endl;
                    return static_cast<int>(ExitCode::fail);
                }
                auto len = ConstDataView(buf.get()).read<LittleEndian<uint32_t>>();
                if (len > (uint32_t)BSONObjMaxInternalSize) {
                    cerr << "Encountered malformed BSON object" << endl;
                    return static_cast<int>(ExitCode::fail);
                }
                inputStream.read(buf.get() + sizeof(uint32_t), len - sizeof(uint32_t));
                if (inputStream.gcount() < (streamsize)(len - sizeof(uint32_t))) {
                    cerr << "Encountered incomplete BSON object" << endl;
                    return static_cast<int>(ExitCode::fail);
                }
                BSONObj readObj(buf.get());
                columnBuilder.append(readObj);
                encoded += len;

                if (encoded > bufferSize && bufferSize > 0) {
                    finishBSONColumn(columnBuilder, lenBuf, outputStream);
                    columnBuilder = BSONColumnBuilder();
                    encoded = 0;
                }
            }
        } else {
            // compressing comma-separated sequence of scalars
            // field count is written first, all lines expected to have same count

            // parse first line separately and count fields
            string line;
            getline(inputStream, line);
            auto numFields = std::count(line.begin(), line.end(), ',') + 1;
            DataView(lenBuf).write<LittleEndian<uint32_t>>(numFields);
            outputStream.write(lenBuf, sizeof(uint32_t));

            do {
                stringstream elementStream(line);
                string elementStr;
                for (int i = 0; i < numFields; ++i) {
                    if (!getline(elementStream, elementStr, ',')) {
                        cerr << "Missing element in csv" << endl;
                        return static_cast<int>(ExitCode::fail);
                    }
                    BSONObjBuilder ob;
                    if (elementStr.empty()) {
                        columnBuilder.skip();
                        encoded++;
                    } else {
                        if (elementStr.find('.') < elementStr.length())
                            ob.appendNumber("", atof(elementStr.c_str()));
                        else
                            ob.appendNumber("", (long long)atol(elementStr.c_str()));
                        const BSONObj obj = ob.done();
                        columnBuilder.append(obj.firstElement());
                        encoded += sizeof(obj.firstElement().size());
                    }

                    if (encoded > bufferSize && bufferSize > 0) {
                        finishBSONColumn(columnBuilder, lenBuf, outputStream);
                        columnBuilder = BSONColumnBuilder();
                        encoded = 0;
                    }
                }
                if (getline(elementStream, elementStr, ',')) {
                    cerr << "Spurious element in csv" << endl;
                    return static_cast<int>(ExitCode::fail);
                }
            } while (getline(inputStream, line));
        }

        if (encoded > 0)
            finishBSONColumn(columnBuilder, lenBuf, outputStream);
    } else {
        if (!csv) {
            // decompressing, works regardless of whether input was obj stream or csv
            // csv will be dumped into a stream of BSONElement
            inputStream.read(lenBuf, sizeof(uint32_t));  // discard field count
            while (true) {
                int len = readBSONColumn(inputStream, lenBuf, buf.get());
                if (len == 0)
                    break;
                if (len == -1)
                    return static_cast<int>(ExitCode::fail);
                BSONColumn col(buf.get(), len);
                for (auto it = col.begin(); it.more(); ++it) {
                    BSONElement ele = *it;
                    outputStream.write(ele.value(), ele.valuesize());
                }
            }
        } else {
            // decompressing back to csv, input must have been csv
            inputStream.read(lenBuf, sizeof(uint32_t));
            auto numFields = ConstDataView(lenBuf).read<LittleEndian<uint32_t>>();
            uint32_t writingIndex = 0;
            if (inputStream.gcount() < (streamsize)sizeof(uint32_t)) {
                cerr << "Missing field count" << endl;
                return static_cast<int>(ExitCode::fail);
            }
            if (numFields == 0) {
                cerr << "Input contains an object sequence" << endl;
                return static_cast<int>(ExitCode::fail);
            }
            while (true) {
                int len = readBSONColumn(inputStream, lenBuf, buf.get());
                if (len == 0)
                    break;
                if (len == -1)
                    return static_cast<int>(ExitCode::fail);
                BSONColumn col(buf.get(), len);
                for (auto it = col.begin(); it.more(); ++it) {
                    BSONElement ele = *it;
                    if (writingIndex > 0)
                        outputStream << ",";
                    switch (ele.type()) {
                        case NumberLong:
                            outputStream << ele.numberLong();
                            break;
                        case NumberInt:
                            outputStream << ele.numberInt();
                            break;
                        case NumberDouble:
                            outputStream << ele.numberDouble();
                            break;
                        default:
                            cerr << "Encountered unsupported BSONType" << endl;
                            return static_cast<int>(ExitCode::fail);
                    }
                    writingIndex++;
                    if (writingIndex >= numFields) {
                        outputStream << endl;
                        writingIndex = 0;
                    }
                }
            }
        }
    }

    return 0;
}
