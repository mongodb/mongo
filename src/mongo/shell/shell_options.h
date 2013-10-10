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

    struct ShellGlobalParams {
        std::string url;
        std::string dbhost;
        std::string port;
        std::vector<std::string> files;

        std::string username;
        std::string password;
        bool usingPassword;
        std::string authenticationMechanism;
        std::string authenticationDatabase;

        bool runShell;
        bool nodb;
        bool norc;

        std::string script;

        bool autoKillOp;

        ShellGlobalParams() : autoKillOp(false) { }
    };

    extern ShellGlobalParams shellGlobalParams;

    Status addMongoShellOptions(moe::OptionSection* options);

    std::string getMongoShellHelp(const StringData& name, const moe::OptionSection& options);

    Status handlePreValidationMongoShellOptions(const moe::Environment& params,
                                                const std::vector<std::string>& args);

    Status storeMongoShellOptions(const moe::Environment& params,
                                  const std::vector<std::string>& args);
}
