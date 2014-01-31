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

#include "mongo/tools/mongooplog_options.h"

#include "mongo/base/status.h"
#include "mongo/util/log.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

    MongoOplogGlobalParams mongoOplogGlobalParams;

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

        options->addOptionChaining("seconds", "seconds,s", moe::Int,
                "seconds to go back default:86400");

        options->addOptionChaining("from", "from", moe::String, "host to pull from");

        options->addOptionChaining("oplogns", "oplogns", moe::String, "ns to pull from")
                                  .setDefault(moe::Value(std::string("local.oplog.rs")));


        return Status::OK();
    }

    void printMongoOplogHelp(std::ostream* out) {
        *out << "Pull and replay a remote MongoDB oplog.\n" << std::endl;
        *out << moe::startupOptions.helpString();
        *out << std::flush;
    }

    bool handlePreValidationMongoOplogOptions(const moe::Environment& params) {
        if (!handlePreValidationGeneralToolOptions(params)) {
            return false;
        }
        if (params.count("help")) {
            printMongoOplogHelp(&std::cout);
            return false;
        }
        return true;
    }

    Status storeMongoOplogOptions(const moe::Environment& params,
                                  const std::vector<std::string>& args) {
        Status ret = storeGeneralToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        if (!hasParam("from")) {
            return Status(ErrorCodes::BadValue, "need to specify --from");
        }
        else {
            mongoOplogGlobalParams.from = getParam("from");
        }

        mongoOplogGlobalParams.seconds = getParam("seconds", 86400);
        mongoOplogGlobalParams.ns = getParam("oplogns");

        return Status::OK();
    }

}
