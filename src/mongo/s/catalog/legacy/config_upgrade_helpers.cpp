/**
 *    Copyright (C) 2012 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/legacy/config_upgrade_helpers.h"

#include <boost/scoped_ptr.hpp>

#include "mongo/client/connpool.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/legacy/cluster_client_internal.h"
#include "mongo/s/type_config_version.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

    using boost::scoped_ptr;
    using std::auto_ptr;
    using std::endl;
    using std::string;

    using mongoutils::str::stream;

    // Custom field used in upgrade state to determine if/where we failed on last upgrade
    const BSONField<bool> inCriticalSectionField("inCriticalSection", false);


    Status preUpgradeCheck(CatalogManager* catalogManager,
                           const VersionType& lastVersionInfo,
                           string minMongosVersion) {

        if (lastVersionInfo.isUpgradeIdSet() && lastVersionInfo.getUpgradeId().isSet()) {
            //
            // Another upgrade failed, so cleanup may be necessary
            //

            BSONObj lastUpgradeState = lastVersionInfo.getUpgradeState();

            bool inCriticalSection;
            string errMsg;
            if (!FieldParser::extract(lastUpgradeState,
                                      inCriticalSectionField,
                                      &inCriticalSection,
                                      &errMsg)) {
                return Status(ErrorCodes::FailedToParse, causedBy(errMsg));
            }

            if (inCriticalSection) {
                // Note: custom message must be supplied by caller
                return Status(ErrorCodes::ManualInterventionRequired, "");
            }
        }

        //
        // Check the versions of other mongo processes in the cluster before upgrade.
        // We can't upgrade if there are active pre-v2.4 processes in the cluster
        //
        return checkClusterMongoVersions(catalogManager, string(minMongosVersion));
    }

    Status commitConfigUpgrade(CatalogManager* catalogManager,
                               int currentVersion,
                               int minCompatibleVersion,
                               int newVersion) {

        // Note: DO NOT CLEAR the config version unless bumping the minCompatibleVersion,
        // we want to save the excludes that were set.

        BSONObjBuilder setObj;
        setObj << VersionType::minCompatibleVersion(minCompatibleVersion);
        setObj << VersionType::currentVersion(newVersion);

        BSONObjBuilder unsetObj;
        unsetObj.append(VersionType::upgradeId(), 1);
        unsetObj.append(VersionType::upgradeState(), 1);
        unsetObj.append("version", 1); // remove deprecated field, no longer supported >= v3.0.

        Status result = catalogManager->update(
                                VersionType::ConfigNS,
                                BSON("_id" << 1 << VersionType::currentVersion(currentVersion)),
                                BSON("$set" << setObj.done() << "$unset" << unsetObj.done()),
                                false,
                                false,
                                NULL);
        if (!result.isOK()) {
            return Status(result.code(),
                          str::stream() << "could not write new version info "
                                        << " and exit critical upgrade section: "
                                        << result.reason());
        }

        return result;
    }

} // namespace mongo
