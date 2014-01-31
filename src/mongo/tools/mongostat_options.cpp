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

#include "mongo/tools/mongostat_options.h"

#include "mongo/base/status.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

    MongoStatGlobalParams mongoStatGlobalParams;

    Status addMongoStatOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addRemoteServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        options->addOptionChaining("noheaders", "noheaders", moe::Switch,
                "don't output column names");

        options->addOptionChaining("rowcount", "rowcount,n", moe::Int,
                "number of stats lines to print (0 for indefinite)")
                                  .setDefault(moe::Value(0));

        options->addOptionChaining("http", "http", moe::Switch,
                "use http instead of raw db connection");

        options->addOptionChaining("discover", "discover", moe::Switch,
                "discover nodes and display stats for all");

        options->addOptionChaining("all", "all", moe::Switch, "all optional fields");

        options->addOptionChaining("sleep", "sleep", moe::Int, "seconds to sleep between samples")
                                  .hidden()
                                  .setSources(moe::SourceCommandLine)
                                  .positional(1, 1);


        return Status::OK();
    }

    void printMongoStatHelp(std::ostream* out) {
        *out << "View live MongoDB performance statistics.\n" << std::endl;
        *out << "usage: mongostat [options] [sleep time]" << std::endl;
        *out << "sleep time: time to wait (in seconds) between calls" << std::endl;
        *out << moe::startupOptions.helpString();
        *out << "\n";
        *out << " Fields\n";
        *out << "   inserts    \t- # of inserts per second (* means replicated op)\n";
        *out << "   query      \t- # of queries per second\n";
        *out << "   update     \t- # of updates per second\n";
        *out << "   delete     \t- # of deletes per second\n";
        *out << "   getmore    \t- # of get mores (cursor batch) per second\n";
        *out << "   command    \t- # of commands per second, on a slave its local|replicated\n";
        *out << "   flushes    \t- # of fsync flushes per second\n";
        *out << "   mapped     \t- amount of data mmaped (total data size) megabytes\n";
        *out << "   vsize      \t- virtual size of process in megabytes\n";
        *out << "   res        \t- resident size of process in megabytes\n";
        *out << "   non-mapped \t- amount virtual memeory less mapped memory (only with --all)\n";
        *out << "   faults     \t- # of pages faults per sec\n";
        *out << "   locked     \t- name of and percent time for most locked database\n";
        *out << "   idx miss   \t- percent of btree page misses (sampled)\n";
        *out << "   qr|qw      \t- queue lengths for clients waiting (read|write)\n";
        *out << "   ar|aw      \t- active clients (read|write)\n";
        *out << "   netIn      \t- network traffic in - bytes\n";
        *out << "   netOut     \t- network traffic out - bytes\n";
        *out << "   conn       \t- number of open connections\n";
        *out << "   set        \t- replica set name\n";
        *out << "   repl       \t- replication type \n";
        *out << "              \t    PRI - primary (master)\n";
        *out << "              \t    SEC - secondary\n";
        *out << "              \t    REC - recovering\n";
        *out << "              \t    UNK - unknown\n";
        *out << "              \t    SLV - slave\n";
        *out << "              b\t    RTR - mongos process (\"router\")\n";
        *out << std::flush;
    }

    bool handlePreValidationMongoStatOptions(const moe::Environment& params) {
        if (!handlePreValidationGeneralToolOptions(params)) {
            return false;
        }
        if (params.count("help")) {
            printMongoStatHelp(&std::cout);
            return false;
        }
        return true;
    }

    Status storeMongoStatOptions(const moe::Environment& params,
                                 const std::vector<std::string>& args) {
        Status ret = storeGeneralToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        if (hasParam("http")) {
            mongoStatGlobalParams.http = true;
            toolGlobalParams.noconnection = true;
        }

        if (hasParam("host") && getParam("host").find(',') != string::npos) {
            toolGlobalParams.noconnection = true;
            mongoStatGlobalParams.many = true;
        }

        if (hasParam("discover")) {
            mongoStatGlobalParams.discover = true;
            mongoStatGlobalParams.many = true;
        }

        mongoStatGlobalParams.showHeaders = !hasParam("noheaders");
        mongoStatGlobalParams.rowCount = getParam("rowcount", 0);
        mongoStatGlobalParams.sleep = getParam("sleep", 1);
        mongoStatGlobalParams.allFields = hasParam("all");

        // Make the default db "admin" if it was not explicitly set
        if (!params.count("db")) {
            toolGlobalParams.db = "admin";
        }

        // end of storage / start of validation

        if (mongoStatGlobalParams.sleep <= 0) {
            return Status(ErrorCodes::BadValue,
                          "Error parsing command line: --sleep must be greater than 0");
        }

        if (mongoStatGlobalParams.rowCount < 0) {
            return Status(ErrorCodes::BadValue,
                          "Error parsing command line: --rowcount (-n) can't be negative");
        }

        return Status::OK();
    }

}
