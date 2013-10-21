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

#include "mongo/tools/mongorestore_options.h"

#include "mongo/base/status.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

    MongoRestoreGlobalParams mongoRestoreGlobalParams;

    typedef moe::OptionDescription OD;
    typedef moe::PositionalOptionDescription POD;

    Status addMongoRestoreOptions(moe::OptionSection* options) {
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

        ret = addBSONToolOptions(options);
        if (!ret.isOK()) {
            return ret;
        }

        options->addOptionChaining("drop", "drop", moe::Switch,
                "drop each collection before import");

        options->addOptionChaining("oplogReplay", "oplogReplay", moe::Switch,
                "replay oplog for point-in-time restore");

        options->addOptionChaining("oplogLimit", "oplogLimit", moe::String,
                "include oplog entries before the provided Timestamp "
                "(seconds[:ordinal]) during the oplog replay; "
                "the ordinal value is optional");

        options->addOptionChaining("keepIndexVersion", "keepIndexVersion", moe::Switch,
                "don't upgrade indexes to newest version");

        options->addOptionChaining("noOptionsRestore", "noOptionsRestore", moe::Switch,
                "don't restore collection options");

        options->addOptionChaining("noIndexRestore", "noIndexRestore", moe::Switch,
                "don't restore indexes");

        options->addOptionChaining("w", "w", moe::Int, "minimum number of replicas per write")
                                  .setDefault(moe::Value(0));

        options->addOptionChaining("dir", "dir", moe::String, "directory to restore from")
                                  .hidden()
                                  .setDefault(moe::Value(std::string("dump")));


        // left in for backwards compatibility
        options->addOptionChaining("indexesLast", "indexesLast", moe::Switch,
                "wait to add indexes (now default)")
                                  .hidden();


        ret = options->addPositionalOption(POD("dir", moe::String, 1));
        if(!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

    void printMongoRestoreHelp(std::ostream* out) {
        *out << "Import BSON files into MongoDB.\n" << std::endl;
        *out << "usage: mongorestore [options] [directory or filename to restore from]"
             << std::endl;
        *out << moe::startupOptions.helpString();
        *out << std::flush;
    }

    bool handlePreValidationMongoRestoreOptions(const moe::Environment& params) {
        if (params.count("help")) {
            printMongoRestoreHelp(&std::cout);
            return true;
        }
        return false;
    }

    Status storeMongoRestoreOptions(const moe::Environment& params,
                                    const std::vector<std::string>& args) {
        Status ret = storeGeneralToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        ret = storeBSONToolOptions(params, args);
        if (!ret.isOK()) {
            return ret;
        }

        mongoRestoreGlobalParams.restoreDirectory = getParam("dir");
        mongoRestoreGlobalParams.drop = hasParam("drop");
        mongoRestoreGlobalParams.keepIndexVersion = hasParam("keepIndexVersion");
        mongoRestoreGlobalParams.restoreOptions = !hasParam("noOptionsRestore");
        mongoRestoreGlobalParams.restoreIndexes = !hasParam("noIndexRestore");
        mongoRestoreGlobalParams.w = getParam( "w" , 0 );
        mongoRestoreGlobalParams.oplogReplay = hasParam("oplogReplay");
        mongoRestoreGlobalParams.oplogLimit = getParam("oplogLimit", "");

        // Make the default db "" if it was not explicitly set
        if (!params.count("db")) {
            toolGlobalParams.db = "";
        }

        return Status::OK();
    }

}
