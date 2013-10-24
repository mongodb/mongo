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

#include "mongo/tools/bsondump_options.h"

#include "mongo/base/status.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

    BSONDumpGlobalParams bsonDumpGlobalParams;

    Status addBSONDumpOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addBSONToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        options->addOptionChaining("type", "type", moe::String, "type of output: json,debug")
                                  .setDefault(moe::Value(std::string("json")));

        options->addOptionChaining("file", "file", moe::String, "path to BSON file to dump")
                                  .hidden()
                                  .setSources(moe::SourceCommandLine)
                                  .positional(1, 1);


        return Status::OK();
    }

    void printBSONDumpHelp(std::ostream* out) {
        *out << "Display BSON objects in a data file.\n" << std::endl;
        *out << "usage: bsondump [options] <bson filename>" << std::endl;
        *out << moe::startupOptions.helpString();
        *out << std::flush;
    }

    bool handlePreValidationBSONDumpOptions(const moe::Environment& params) {
        if (params.count("help")) {
            printBSONDumpHelp(&std::cout);
            return false;
        }
        return true;
    }

    Status storeBSONDumpOptions(const moe::Environment& params,
                                const std::vector<std::string>& args) {
        Status ret = storeGeneralToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        ret = storeBSONToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        // BSONDump never has a db connection
        toolGlobalParams.noconnection = true;

        bsonDumpGlobalParams.type = getParam("type");
        bsonDumpGlobalParams.file = getParam("file");

        // Make the default db "" if it was not explicitly set
        if (!params.count("db")) {
            toolGlobalParams.db = "";
        }

        // bsondump always outputs data to stdout, so we can't send messages there
        toolGlobalParams.canUseStdout = false;

        return Status::OK();
    }

}
