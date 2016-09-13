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

#include <boost/optional.hpp>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/rpc/protocol.h"

namespace mongo {

namespace optionenvironment {
class OptionSection;
class Environment;
}  // namespace optionenvironment

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
    std::string gssapiServiceName;
    std::string gssapiHostName;

    bool runShell;
    bool nodb;
    bool norc;
    bool nojit = false;
    bool javascriptProtection = true;

    std::string script;

    bool autoKillOp = false;
    bool useWriteCommandsDefault = true;

    std::string writeMode = "commands";
    std::string readMode = "compatibility";

    boost::optional<rpc::ProtocolSet> rpcProtocols = boost::none;
};

extern ShellGlobalParams shellGlobalParams;

Status addMongoShellOptions(moe::OptionSection* options);

std::string getMongoShellHelp(StringData name, const moe::OptionSection& options);

/**
 * Handle options that should come before validation, such as "help".
 *
 * Returns false if an option was found that implies we should prematurely exit with success.
 */
bool handlePreValidationMongoShellOptions(const moe::Environment& params,
                                          const std::vector<std::string>& args);

Status storeMongoShellOptions(const moe::Environment& params, const std::vector<std::string>& args);
}
