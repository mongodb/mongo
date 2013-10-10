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

    struct MongoOplogGlobalParams {
        int seconds;
        std::string from;
        std::string ns;
    };

    extern MongoOplogGlobalParams mongoOplogGlobalParams;

    Status addMongoOplogOptions(moe::OptionSection* options);

    void printMongoOplogHelp(std::ostream* out);

    Status handlePreValidationMongoOplogOptions(const moe::Environment& params);

    Status storeMongoOplogOptions(const moe::Environment& params,
                                  const std::vector<std::string>& args);
}
