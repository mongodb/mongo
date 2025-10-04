/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/hex.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_description.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/value.h"
#include "mongo/util/str.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
namespace moe = mongo::optionenvironment;

const std::string kUsage = R"(
    Usage: ksdecode [-o FORMAT] [-p KEY_PATTERN] [-t TYPEBITS] [-r RID_TYPE] [-q | -i] [-a]
                    [KEYSTRING]

    KEYSTRING is a hexadecimal representation of a key string. For example: 2b06d4f7040010. If no
    key string is provided, then the input is streamed line-by-line from stdin. If a key string can  
    be decoded, it will be printed. If a keystring is encountered that cannot be decoded, the 
    program will exit with an error unless -q or -l are specified. Unless -a is specified, the only
    output will be successfully decoded keystrings.

    Options:
    -o FORMAT: One of the following output formats:
      * "explain" (default) provides a detailed multi-line explaination of the key string
      * "bson" provides a single-line conversion of the key string to BSON 

    -p KEY_PATTERN: A JSON object representing the key pattern of this index. For example: 
    {lastName: -1, firstName: 1}. Keystrings contain enough information to infer the number of
    fields, but not whether a field is ascending or descending. If no key pattern is provided, all
    fields are inferred to be ascending and field names are subsituted with empty strings.

    Tip: a failure to decode with "unknown type" most likely indicates that the keystring does not
    decode with the provided (or default) key pattern. The field orderings may be incorrect.

    -t TYPEBITS: A hexadecimal representation of type bits. The default is all zeros. Without type
      bits, the printed object will have the correct value, but not necessarily the correct type.
      For example, a value "42.0" originally stored as a float may be printed as "42" without
      the correct type bits. This option will not behave correctly in streaming mode, as it only
      applies to a single keystring.

    Tip: For non-_id indexes in MongoDB, the keystring (with record id) is stored in the key of the
      WT table, and the type bits are stored in the value.

    -r RID_TYPE (default=none): The RecordId format present in this KeyString. It may be one of
      "long", "string" or "none". Note that only non-_id indexes have RecordIds appended to the end.

    -l: Log errors (default=false): In streaming mode, ignore errors and log them. May not also
      be set in quiet mode (-q).

    -q: Quiet mode (default=false): Decoding errors for individual keystrings (or lines in streaming
      mode) will be ignored and no decoded output will be displayed for a keystring. May not also be
      set when logging errors (-l).

    -a: Append (default=false): When data is streamed from stdin, append the decoded output to the
      end of the input line. When not specified, only successfully decoded key strings appear in the
      output. 

    Examples:

    Decode a key without any key pattern. Note that this key is from an _id index which does not
    store RecordIds in the keystring. Additionally, this will only decode correctly if the original
    key pattern had only ascending keys. An error will occur otherwise.

        ksdecode 64622f09d31e59faf20f1a387d04

    Decode with no type bits on an index with two fields in different orders:

        ksdecode -p '{a: 1, b: -1}' -r long 2b06d4f7040010

    Decode with two fields in different orders and type bits, with BSON output format:

        ksdecode -p '{a: 1, b: -1}' -t 02 -r long -o bson 2b54eb040018

    Using input from a dump from a non-_id WiredTiger index, decode to BSON (-o bson),
    suppress errors (-q), and append the successfully decoded keystrings to the end of each input
    line (-a). Note that this will not be able to correctly use type bits information, which is
    output on a separate line, to fully decode the original BSON object.

        wt -h /data/db/ dump -x file:index.wt | ksdecode -p '{a: 1, b: -1}' -r long -o bson -a -q

    Limitations:
      * Only supports Keystring V1 keys
      * Does not provide detailed type bits analysis for Decimal datatypes
)";

void printUsage() {
    std::cout << kUsage << std::endl;
}

MONGO_COMPILER_NORETURN void exitWithUsage(std::string message) {
    std::cout << "Error: " << message << std::endl;
    printUsage();
    exit(1);
}

enum class OutputFormat { kBson, kExplain };

struct KSDecodeOptions {
    OutputFormat outputFormat = OutputFormat::kExplain;
    BSONObj keyPattern;
    // When empty, read from STDIN
    std::string binKeyString;
    std::string binTypeBits;
    boost::optional<KeyFormat> keyFormat;
    bool quiet = false;
    bool logErrors = false;
    bool append = false;
};

void decode(const KSDecodeOptions& options) {
    key_string::TypeBits typeBits(key_string::Version::kLatestVersion);
    if (!options.binTypeBits.empty()) {
        BufReader reader(options.binTypeBits.c_str(), options.binTypeBits.size());
        typeBits.resetFromBuffer(&reader);
    }

    auto builder = key_string::HeapBuilder(key_string::Version::kLatestVersion);
    builder.resetFromBuffer(options.binKeyString);

    if (OutputFormat::kExplain == options.outputFormat) {
        std::cout << key_string::explain(
            builder.getView(), options.keyPattern, typeBits, options.keyFormat);
    } else if (OutputFormat::kBson == options.outputFormat) {
        auto bson =
            key_string::toBsonSafe(builder.getView(), Ordering::make(options.keyPattern), typeBits);
        auto rehydrated = key_string::rehydrateKey(options.keyPattern, bson);
        str::stream out;
        if (options.binKeyString.size() >= 2 && options.keyFormat) {
            BSONObjBuilder bob(rehydrated);
            RecordId recordId =
                key_string::decodeRecordIdAtEnd(options.binKeyString, *options.keyFormat);
            recordId.serializeToken("$recordId", &bob);
            out << bob.obj();
        } else {
            out << rehydrated;
        }
        std::cout << out << std::endl;
    }
}

void decodeFromInputsOrStream(const KSDecodeOptions& options) {
    if (!options.binKeyString.empty()) {
        try {
            decode(options);
        } catch (const std::exception& e) {
            if (!options.quiet) {
                exitWithUsage(str::stream() << "Exception decoding: " << e.what());
            }
        }
        return;
    }

    // Streaming mode
    for (std::string line; std::getline(std::cin, line);) {
        if (options.append) {
            if (line.empty()) {
                std::cout << std::endl;
            } else {
                std::cout << line << " ";
            }
        }

        KSDecodeOptions optCopy = options;
        try {
            optCopy.binKeyString = hexblob::decode(line);
            if (!optCopy.binKeyString.empty()) {
                decode(optCopy);
            }
        } catch (const std::exception& e) {
            if (options.logErrors) {
                std::cout << "Error decoding: " << e.what() << std::endl;
            } else if (!options.quiet) {
                exitWithUsage(str::stream() << "Exception decoding: " << e.what());
            } else if (options.append) {
                std::cout << std::endl;
            }
        }
    }
}

int ksDecodeMain(int argc, char* argv[]) try {
    constexpr auto OptionUsage =
        moe::OptionSection::OptionParserUsageType::BaseServerOptionsException;
    moe::OptionSection opts;
    opts.addOptionChaining("keystring", "keystring", moe::String, "KeyString", {}, {}, OptionUsage)
        .hidden()
        .positional(1, 1);
    opts.addOptionChaining("help", "h", moe::Switch, "Help output", {}, {}, OptionUsage);
    opts.addOptionChaining("output", "o", moe::String, "Output format", {}, {}, OptionUsage);
    opts.addOptionChaining("pattern", "p", moe::String, "Key pattern", {}, {}, OptionUsage);
    opts.addOptionChaining("typeBits", "t", moe::String, "Type bits", {}, {}, OptionUsage);
    opts.addOptionChaining("recordId", "r", moe::String, "RecordId type", {}, {}, OptionUsage);
    opts.addOptionChaining(
        "logErrors", "l", moe::Switch, "Ignore and log errors", {}, {}, OptionUsage);
    opts.addOptionChaining("quiet", "q", moe::Switch, "Quiet", {}, {}, OptionUsage);
    opts.addOptionChaining("append", "a", moe::Switch, "Append", {}, {}, OptionUsage);

    moe::OptionsParser parser;
    moe::Environment environment;
    std::vector<std::string> argvVec(argv, argv + argc);
    uassertStatusOK(parser.run(opts, argvVec, &environment));

    KSDecodeOptions options;

    if (environment.count("help")) {
        printUsage();
        return 0;
    }

    if (environment.count("keystring")) {
        options.binKeyString = hexblob::decode(environment["keystring"].as<std::string>());
    }

    if (environment.count("output")) {
        auto strVal = environment["output"].as<std::string>();
        if (str::equalCaseInsensitive(strVal, "explain")) {
            options.outputFormat = OutputFormat::kExplain;
        } else if (str::equalCaseInsensitive(strVal, "bson")) {
            options.outputFormat = OutputFormat::kBson;
        } else {
            exitWithUsage("Unknown output format");
        }
    }

    if (environment.count("pattern")) {
        std::string strVal = environment["pattern"].as<std::string>();
        options.keyPattern = fromjson(strVal);
    }

    if (environment.count("typeBits")) {
        options.binTypeBits = hexblob::decode(environment["typeBits"].as<std::string>());
    }

    if (environment.count("recordId")) {
        std::string strVal = environment["recordId"].as<std::string>();
        if (str::equalCaseInsensitive(strVal, "string")) {
            options.keyFormat.emplace(KeyFormat::String);
        } else if (str::equalCaseInsensitive(strVal, "long")) {
            options.keyFormat.emplace(KeyFormat::Long);
        } else if (!str::equalCaseInsensitive(strVal, "none")) {
            exitWithUsage("Unknown RecordId format");
        }
    }

    if (environment.count("logErrors")) {
        options.logErrors = true;
    }

    // Quiet
    if (environment.count("quiet")) {
        options.quiet = true;
    }

    if (options.quiet && options.logErrors) {
        uasserted(ErrorCodes::Error::InvalidOptions, "Cannot provide both -l and -q options");
    }

    // Append
    if (environment.count("append")) {
        options.append = true;
    }

    decodeFromInputsOrStream(options);
    return 0;
} catch (const std::exception& e) {
    exitWithUsage(str::stream() << "Caught exception: " << e.what());
}
}  // namespace
}  // namespace mongo

int main(int argc, char* argv[]) {
    return mongo::ksDecodeMain(argc, argv);
}
