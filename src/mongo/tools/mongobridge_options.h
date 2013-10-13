/*
 *    Copyright (C) 2013 10gen Inc.
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

namespace mongo {

    namespace optionenvironment {
        class OptionSection;
        class Environment;
    } // namespace optionenvironment

    namespace moe = mongo::optionenvironment;

    struct MongoBridgeGlobalParams {
        int port;
        int delay;
        string destUri;

        MongoBridgeGlobalParams() : port(0), delay(0) { }
    };

    extern MongoBridgeGlobalParams mongoBridgeGlobalParams;

    Status addMongoBridgeOptions(moe::OptionSection* options);

    void printMongoBridgeHelp(std::ostream* out);

    Status handlePreValidationMongoBridgeOptions(const moe::Environment& params);

    Status storeMongoBridgeOptions(const moe::Environment& params,
                                   const std::vector<std::string>& args);
}
