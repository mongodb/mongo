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

#include <string>
#include <vector>

#include "mongo/base/status.h"

namespace mongo {

    namespace optionenvironment {
        class OptionSection;
        class Environment;
    } // namespace optionenvironment

    namespace moe = mongo::optionenvironment;

    struct FrameworkGlobalParams {
        unsigned perfHist;
        unsigned long long seed;
        int runsPerTest;
        std::string dbpathSpec;
        std::vector<std::string> suites;
        std::string filter;
    };

    extern FrameworkGlobalParams frameworkGlobalParams;

    Status addTestFrameworkOptions(moe::OptionSection* options);

    std::string getTestFrameworkHelp(const StringData& name, const moe::OptionSection& options);

    Status preValidationTestFrameworkOptions(const moe::Environment& params,
                                             const std::vector<std::string>& args);

    Status storeTestFrameworkOptions(const moe::Environment& params,
                                     const std::vector<std::string>& args);
}
