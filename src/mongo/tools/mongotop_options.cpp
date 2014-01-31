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

#include "mongo/tools/mongotop_options.h"

#include "mongo/base/status.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

    MongoTopGlobalParams mongoTopGlobalParams;

    Status addMongoTopOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addRemoteServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        options->addOptionChaining("locks", "locks", moe::Switch,
                "use db lock info instead of top");

        options->addOptionChaining("sleep", "sleep", moe::Int, "seconds to sleep between samples")
                                  .hidden()
                                  .setSources(moe::SourceCommandLine)
                                  .positional(1, 1);


        return Status::OK();
    }

    void printMongoTopHelp(std::ostream* out) {
        *out << "View live MongoDB collection statistics.\n" << std::endl;
        *out << moe::startupOptions.helpString();
        *out << std::flush;
    }

    bool handlePreValidationMongoTopOptions(const moe::Environment& params) {
        if (!handlePreValidationGeneralToolOptions(params)) {
            return false;
        }
        if (params.count("help")) {
            printMongoTopHelp(&std::cout);
            return false;
        }
        return true;
    }

    Status storeMongoTopOptions(const moe::Environment& params,
                                const std::vector<std::string>& args) {
        Status ret = storeGeneralToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        mongoTopGlobalParams.sleep = getParam("sleep", 1);
        mongoTopGlobalParams.useLocks = hasParam("locks");

        // Make the default db "admin" if it was not explicitly set
        if (!params.count("db")) {
            toolGlobalParams.db = "admin";
        }

        return Status::OK();
    }

}
