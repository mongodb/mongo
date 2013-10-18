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

    struct MongoFilesGlobalParams {
        std::string localFile;
        std::string contentType;
        bool replace;
        std::string command;
        std::string gridFSFilename;
    };

    extern MongoFilesGlobalParams mongoFilesGlobalParams;

    Status addMongoFilesOptions(moe::OptionSection* options);

    void printMongoFilesHelp(std::ostream* out);

    bool handlePreValidationMongoFilesOptions(const moe::Environment& params);

    Status storeMongoFilesOptions(const moe::Environment& params,
                                  const std::vector<std::string>& args);
}
