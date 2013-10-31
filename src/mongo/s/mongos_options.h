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

#include "mongo/base/status.h"
#include "mongo/db/server_options.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"

namespace mongo {

    namespace optionenvironment {
        class OptionSection;
        class Environment;
    } // namespace optionenvironment

    namespace moe = mongo::optionenvironment;

    struct MongosGlobalParams {
        std::vector<std::string> configdbs;
        bool upgrade;

        MongosGlobalParams() :
            upgrade(false)
        { }
    };

    extern MongosGlobalParams mongosGlobalParams;

    Status addMongosOptions(moe::OptionSection* options);

    void printMongosHelp(const moe::OptionSection& options);

    /**
     * Handle options that should come before validation, such as "help".
     *
     * Returns false if an option was found that implies we should prematurely exit with success.
     */
    bool handlePreValidationMongosOptions(const moe::Environment& params,
                                            const std::vector<std::string>& args);

    Status storeMongosOptions(const moe::Environment& params, const std::vector<std::string>& args);

    bool isMongos();
}
