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

namespace mongo {

    namespace optionenvironment {
        class OptionSection;
        class Environment;
    } // namespace optionenvironment

    namespace moe = mongo::optionenvironment;

    struct ToolGlobalParams {

        ToolGlobalParams() : canUseStdout(true), hostSet(false), portSet(false) { }
        std::string name;

        std::string db;
        std::string coll;

        std::string username;
        std::string password;
        std::string authenticationDatabase;
        std::string authenticationMechanism;

        bool quiet;
        bool canUseStdout;
        bool noconnection;

        std::vector<std::string> fields;
        bool fieldsSpecified;

        std::string host; // --host
        bool hostSet;
        std::string port; // --port
        bool portSet;
        std::string connectionString; // --host and --port after processing
        std::string dbpath;
        bool useDirectClient;
    };

    extern ToolGlobalParams toolGlobalParams;

    struct BSONToolGlobalParams {
        bool objcheck;
        std::string filter;
        bool hasFilter;
    };

    extern BSONToolGlobalParams bsonToolGlobalParams;

    Status addGeneralToolOptions(moe::OptionSection* options);

    Status addRemoteServerToolOptions(moe::OptionSection* options);

    Status addLocalServerToolOptions(moe::OptionSection* options);

    Status addSpecifyDBCollectionToolOptions(moe::OptionSection* options);

    Status addBSONToolOptions(moe::OptionSection* options);

    Status addFieldOptions(moe::OptionSection* options);

    // Legacy interface for getting options in tools
    // TODO: Remove this when we use the new interface everywhere
    std::string getParam(std::string name, string def="");
    int getParam(std::string name, int def);
    bool hasParam(std::string name);

    Status handlePreValidationGeneralToolOptions(const moe::Environment& params);

    Status storeGeneralToolOptions(const moe::Environment& params,
                                   const std::vector<std::string>& args);

    Status storeFieldOptions(const moe::Environment& params,
                             const std::vector<std::string>& args);

    Status storeBSONToolOptions(const moe::Environment& params,
                                const std::vector<std::string>& args);
}
