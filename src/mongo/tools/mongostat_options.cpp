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

#include "mongo/tools/mongostat_options.h"

#include "mongo/base/status.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_description.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/startup_option_init.h"

namespace mongo {

    MongoStatGlobalParams mongoStatGlobalParams;

    typedef moe::OptionDescription OD;
    typedef moe::PositionalOptionDescription POD;

    Status addMongoStatOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addRemoteServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("noheaders", "noheaders", moe::Switch,
                    "don't output column names", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("rowcount", "rowcount,n", moe::Int,
                    "number of stats lines to print (0 for indefinite)", true,
                    moe::Value(0)));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("http", "http", moe::Switch,
                    "use http instead of raw db connection", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("discover", "discover", moe::Switch,
                    "discover nodes and display stats for all", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("all", "all", moe::Switch,
                    "all optional fields", true));
        if(!ret.isOK()) {
            return ret;
        }

        ret = options->addPositionalOption(POD( "sleep", moe::Int, 1 ));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    void printMongoStatHelp(const moe::OptionSection options, std::ostream* out) {
        *out << "View live MongoDB performance statistics.\n" << std::endl;
        *out << "usage: mongostat [options] [sleep time]" << std::endl;
        *out << "sleep time: time to wait (in seconds) between calls" << std::endl;
        *out << options.helpString();
        *out << "\n";
        *out << " Fields\n";
        *out << "   inserts  \t- # of inserts per second (* means replicated op)\n";
        *out << "   query    \t- # of queries per second\n";
        *out << "   update   \t- # of updates per second\n";
        *out << "   delete   \t- # of deletes per second\n";
        *out << "   getmore  \t- # of get mores (cursor batch) per second\n";
        *out << "   command  \t- # of commands per second, on a slave its local|replicated\n";
        *out << "   flushes  \t- # of fsync flushes per second\n";
        *out << "   mapped   \t- amount of data mmaped (total data size) megabytes\n";
        *out << "   vsize    \t- virtual size of process in megabytes\n";
        *out << "   res      \t- resident size of process in megabytes\n";
        *out << "   faults   \t- # of pages faults per sec\n";
        *out << "   locked   \t- name of and percent time for most locked database\n";
        *out << "   idx miss \t- percent of btree page misses (sampled)\n";
        *out << "   qr|qw    \t- queue lengths for clients waiting (read|write)\n";
        *out << "   ar|aw    \t- active clients (read|write)\n";
        *out << "   netIn    \t- network traffic in - bytes\n";
        *out << "   netOut   \t- network traffic out - bytes\n";
        *out << "   conn     \t- number of open connections\n";
        *out << "   set      \t- replica set name\n";
        *out << "   repl     \t- replication type \n";
        *out << "            \t    PRI - primary (master)\n";
        *out << "            \t    SEC - secondary\n";
        *out << "            \t    REC - recovering\n";
        *out << "            \t    UNK - unknown\n";
        *out << "            \t    SLV - slave\n";
        *out << "            \t    RTR - mongos process (\"router\")\n";
        *out << std::flush;
    }

    Status handlePreValidationMongoStatOptions(const moe::Environment& params) {
        if (toolsParsedOptions.count("help")) {
            printMongoStatHelp(toolsOptions, &std::cout);
            ::_exit(0);
        }
        return Status::OK();
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
        if (!toolsParsedOptions.count("db")) {
            toolGlobalParams.db = "admin";
        }

        // Make the default db "admin" if it was not explicitly set
        if (!toolsParsedOptions.count("db")) {
            toolGlobalParams.db = "admin";
        }

        return Status::OK();
    }

    MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(MongoStatOptions)(InitializerContext* context) {
        return addMongoStatOptions(&toolsOptions);
    }

    MONGO_STARTUP_OPTIONS_PARSE(MongoStatOptions)(InitializerContext* context) {
        moe::OptionsParser parser;
        Status ret = parser.run(toolsOptions, context->args(), context->env(),
                                &toolsParsedOptions);
        if (!ret.isOK()) {
            std::cerr << ret.reason() << std::endl;
            std::cerr << "try '" << context->args()[0]
                      << " --help' for more information" << std::endl;
            ::_exit(EXIT_BADOPTIONS);
        }
        return Status::OK();
    }

    MONGO_STARTUP_OPTIONS_VALIDATE(MongoStatOptions)(InitializerContext* context) {
        Status ret = handlePreValidationMongoStatOptions(toolsParsedOptions);
        if (!ret.isOK()) {
            return ret;
        }
        ret = toolsParsedOptions.validate();
        if (!ret.isOK()) {
            return ret;
        }
        return Status::OK();
    }

    MONGO_STARTUP_OPTIONS_STORE(MongoStatOptions)(InitializerContext* context) {
        return storeMongoStatOptions(toolsParsedOptions, context->args());
    }
}
