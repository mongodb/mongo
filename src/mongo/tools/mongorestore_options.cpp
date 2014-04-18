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

#include "mongo/tools/mongorestore_options.h"

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {

    MongoRestoreGlobalParams mongoRestoreGlobalParams;

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

        options->addOptionChaining("restoreDbUsersAndRoles", "restoreDbUsersAndRoles", moe::Switch,
                "Restore user and role definitions for the given database")
                        .requires("db").incompatibleWith("collection");

        options->addOptionChaining(
                "tempUsersCollection", "tempUsersCollection", moe::String,
                        "Collection in which to temporarily store user data during the restore")
                .hidden()
                .setDefault(moe::Value(
                        AuthorizationManager::defaultTempUsersCollectionNamespace.toString()));

        options->addOptionChaining(
                "tempRolesCollection", "tempRolesCollection", moe::String,
                       "Collection in which to temporarily store role data during the restore")
                .hidden()
                .setDefault(moe::Value(
                        AuthorizationManager::defaultTempRolesCollectionNamespace.toString()));

        options->addOptionChaining("w", "w", moe::Int, "minimum number of replicas per write")
                                  .setDefault(moe::Value(0));

        options->addOptionChaining("dir", "dir", moe::String, "directory to restore from")
                                  .hidden()
                                  .setDefault(moe::Value(std::string("dump")))
                                  .positional(1, 1);


        // left in for backwards compatibility
        options->addOptionChaining("indexesLast", "indexesLast", moe::Switch,
                "wait to add indexes (now default)")
                                  .hidden();


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
        if (!handlePreValidationGeneralToolOptions(params)) {
            return false;
        }
        if (params.count("help")) {
            printMongoRestoreHelp(&std::cout);
            return false;
        }
        return true;
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
        mongoRestoreGlobalParams.tempUsersColl = getParam("tempUsersCollection");
        mongoRestoreGlobalParams.tempRolesColl = getParam("tempRolesCollection");

        // Make the default db "" if it was not explicitly set
        if (!params.count("db")) {
            toolGlobalParams.db = "";
        }

        if (hasParam("restoreDbUsersAndRoles") && toolGlobalParams.db == "admin") {
            return Status(ErrorCodes::BadValue,
                          "Cannot provide --restoreDbUsersAndRoles when restoring the admin db as "
                          "user and role definitions for the whole server are restored by "
                          "default (if present) when restoring the admin db");
        }

        // Always restore users and roles if doing a full restore.  If doing a db restore, only
        // restore users and roles if --restoreDbUsersAndRoles provided or you're restoring the
        // admin db
        mongoRestoreGlobalParams.restoreUsersAndRoles = hasParam("restoreDbUsersAndRoles") ||
                (toolGlobalParams.db.empty() && toolGlobalParams.coll.empty()) ||
                (toolGlobalParams.db == "admin" && toolGlobalParams.coll.empty());

        return Status::OK();
    }

}
