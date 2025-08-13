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
#include "mongo/bson/util/bson_corpus.h"
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
            boost::program_options::value<std::string>()->default_value("bsoncolumncorpus.bson"),
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

    for (int i = 0; i < mongo::bson::corpusSize(); ++i) {
        auto corpusObject = mongo::bson::getCorpusObject(i);
        outputStream.write(corpusObject.data(), corpusObject.size());
    }

    return 0;
}
