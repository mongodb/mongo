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

#include "mongo/tools/mongooplog_options.h"

#include "mongo/base/status.h"
#include "mongo/util/log.h"
#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

    MongoOplogGlobalParams mongoOplogGlobalParams;

    typedef moe::OptionDescription OD;
    typedef moe::PositionalOptionDescription POD;

    Status addMongoOplogOptions(moe::OptionSection* options) {
        Status ret = addGeneralToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addRemoteServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addLocalServerToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addSpecifyDBCollectionToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        ret = options->addOption(OD("seconds", "seconds,s", moe::Int ,
                    "seconds to go back default:86400", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("from", "from", moe::String , "host to pull from", true));
        if(!ret.isOK()) {
            return ret;
        }
        ret = options->addOption(OD("oplogns", "oplogns", moe::String ,
                    "ns to pull from" , true, moe::Value(std::string("local.oplog.rs"))));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    void printMongoOplogHelp(std::ostream* out) {
        *out << "Pull and replay a remote MongoDB oplog.\n" << std::endl;
        *out << moe::startupOptions.helpString();
        *out << std::flush;
    }

    Status handlePreValidationMongoOplogOptions(const moe::Environment& params) {
        if (params.count("help")) {
            printMongoOplogHelp(&std::cout);
            ::_exit(0);
        }
        return Status::OK();
    }

    Status storeMongoOplogOptions(const moe::Environment& params,
                                  const std::vector<std::string>& args) {
        Status ret = storeGeneralToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        if (!hasParam("from")) {
            log() << "need to specify --from" << std::endl;
            ::_exit(-1);
        }
        else {
            mongoOplogGlobalParams.from = getParam("from");
        }

        mongoOplogGlobalParams.seconds = getParam("seconds", 86400);
        mongoOplogGlobalParams.ns = getParam("oplogns");

        return Status::OK();
    }

    MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(MongoOplogOptions)(InitializerContext* context) {
        return addMongoOplogOptions(&moe::startupOptions);
    }

    MONGO_STARTUP_OPTIONS_VALIDATE(MongoOplogOptions)(InitializerContext* context) {
        Status ret = handlePreValidationMongoOplogOptions(moe::startupOptionsParsed);
        if (!ret.isOK()) {
            return ret;
        }
        ret = moe::startupOptionsParsed.validate();
        if (!ret.isOK()) {
            return ret;
        }
        return Status::OK();
    }

    MONGO_STARTUP_OPTIONS_STORE(MongoOplogOptions)(InitializerContext* context) {
        return storeMongoOplogOptions(moe::startupOptionsParsed, context->args());
    }
}
