/*
 *    Copyright (C) 2010 10gen Inc.
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
 */

#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/tools/tool_options.h"

namespace mongo {

    struct MongoImportGlobalParams {
        bool ignoreBlanks;
        std::string type;
        std::string filename;
        bool drop;
        bool headerLine;
        bool upsert;
        std::vector<std::string> upsertFields;
        bool stopOnError;
        bool jsonArray;
        bool doimport;
    };

    extern MongoImportGlobalParams mongoImportGlobalParams;

    Status addMongoImportOptions(moe::OptionSection* options);

    void printMongoImportHelp(const moe::OptionSection options, std::ostream* out);

    Status handlePreValidationMongoImportOptions(const moe::Environment& params);

    Status storeMongoImportOptions(const moe::Environment& params,
                                   const std::vector<std::string>& args);
}
