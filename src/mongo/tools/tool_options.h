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
        std::string gssapiServiceName;
        std::string gssapiHostName;

        bool quiet;
        bool canUseStdout;
        bool noconnection;

        std::vector<std::string> fields;
        bool fieldsSpecified;

        std::string host; // --host
        bool hostSet;
        int port; // --port
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

    /**
     * Handle options that should come before validation, such as "help".
     *
     * Returns false if an option was found that implies we should prematurely exit with success.
     */
    bool handlePreValidationGeneralToolOptions(const moe::Environment& params);

    Status storeGeneralToolOptions(const moe::Environment& params,
                                   const std::vector<std::string>& args);

    Status storeFieldOptions(const moe::Environment& params,
                             const std::vector<std::string>& args);

    Status storeBSONToolOptions(const moe::Environment& params,
                                const std::vector<std::string>& args);
}
