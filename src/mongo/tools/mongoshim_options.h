// mongoshim_options.h

/*
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/tools/shim_mode.h"
#include "mongo/tools/tool_options.h"

namespace mongo {

    struct MongoShimGlobalParams {

        MongoShimGlobalParams() : mode(ShimMode::kNumShimModes) { }

        // Mongoshim can run in one mode per invocation.
        // See shim_mode.h for list of supported modes.
        ShimMode mode;
        bool drop;

        std::vector<std::string> upsertFields;

        std::string query;

        // If --in is specified, reads documents from 'outputFile' instead of stdin.
        // Used primarily for testing.
        std::string inputFile;
        bool inputFileSpecified;

        // If --inputDocuments is specified, documents will be parsed from option value
        // instead of being read from stdin.
        // Used primarily for testing.
        BSONObj inputDocuments;

        // If --out is specified, writes output to 'outputFile' instead of stdout.
        // Used primarily for testing.
        std::string outputFile;
        bool outputFileSpecified;

        unsigned int skip;
        unsigned int limit;
        std::string sort;
    };

    extern MongoShimGlobalParams mongoShimGlobalParams;

    Status addMongoShimOptions(moe::OptionSection* options);

    void printMongoShimHelp(std::ostream* out);

    /**
     * Handle options that should come before validation, such as "help".
     *
     * Returns false if an option was found that implies we should prematurely exit with success.
     */
    bool handlePreValidationMongoShimOptions(const moe::Environment& params);

    Status storeMongoShimOptions(const moe::Environment& params,
                                   const std::vector<std::string>& args);
}
