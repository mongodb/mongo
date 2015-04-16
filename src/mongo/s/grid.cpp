/**
 *    Copyright (C) 2010-2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/grid.h"

#include "mongo/base/status_with.h"
#include "mongo/client/connpool.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/config.h"
#include "mongo/s/type_collection.h"
#include "mongo/s/type_settings.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

    using boost::shared_ptr;
    using std::endl;
    using std::map;
    using std::set;
    using std::string;
    using std::vector;

    MONGO_FP_DECLARE(neverBalance);


    Grid::Grid() : _allowLocalShard(true) {

    }

    void Grid::setCatalogManager(std::unique_ptr<CatalogManager> catalogManager) {
        invariant(!_catalogManager);
        invariant(!_catalogCache);

        _catalogManager = std::move(catalogManager);
        _catalogCache = std::make_unique<CatalogCache>(_catalogManager.get());
    }

    StatusWith<shared_ptr<DBConfig>> Grid::implicitCreateDb(const std::string& dbName) {
        auto status = catalogCache()->getDatabase(dbName);
        if (status.isOK()) {
            return status;
        }

        if (status == ErrorCodes::DatabaseNotFound) {
            auto statusCreateDb = catalogManager()->createDatabase(dbName, NULL);
            if (statusCreateDb.isOK() || statusCreateDb == ErrorCodes::NamespaceExists) {
                return catalogCache()->getDatabase(dbName);
            }

            return statusCreateDb;
        }

        return status;
    }

    bool Grid::allowLocalHost() const {
        return _allowLocalShard;
    }

    void Grid::setAllowLocalHost( bool allow ) {
        _allowLocalShard = allow;
    }

    /*
     * Returns whether balancing is enabled, with optional namespace "ns" parameter for balancing on a particular
     * collection.
     */

    bool Grid::shouldBalance(const SettingsType& balancerSettings) const {
        // Allow disabling the balancer for testing
        if (MONGO_FAIL_POINT(neverBalance)) return false;

        if (balancerSettings.isBalancerStoppedSet() && balancerSettings.getBalancerStopped()) {
            return false;
        }

        if (balancerSettings.isBalancerActiveWindowSet()) {
            boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
            return _inBalancingWindow(balancerSettings.getBalancerActiveWindow(), now);
        }

        return true;
    }

    bool Grid::getBalancerSettings(SettingsType* settings, string* errMsg) const {
        BSONObj balancerDoc;
        ScopedDbConnection conn(configServer.getPrimary().getConnString(), 30);

        try {
            balancerDoc = conn->findOne(SettingsType::ConfigNS,
                                        BSON(SettingsType::key("balancer")));
            conn.done();
        }
        catch (const DBException& ex) {
            *errMsg = str::stream() << "failed to read balancer settings from " << conn.getHost()
                                    << ": " << causedBy(ex);
            return false;
        }

        return settings->parseBSON(balancerDoc, errMsg);
    }

    bool Grid::getConfigShouldBalance() const {
        SettingsType balSettings;
        string errMsg;

        if (!getBalancerSettings(&balSettings, &errMsg)) {
            warning() << errMsg;
            return false;
        }

        if (!balSettings.isKeySet()) {
            // Balancer settings doc does not exist. Default to yes.
            return true;
        }

        return shouldBalance(balSettings);
    }

    bool Grid::getCollShouldBalance(const std::string& ns) const {
        BSONObj collDoc;
        ScopedDbConnection conn(configServer.getPrimary().getConnString(), 30);

        try {
            collDoc = conn->findOne(CollectionType::ConfigNS, BSON(CollectionType::ns(ns)));
            conn.done();
        }
        catch (const DBException& e){
            conn.kill();
            warning() << "could not determine whether balancer should be running, error getting"
                      << "config data from " << conn.getHost() << causedBy(e) << endl;
            // if anything goes wrong, we shouldn't try balancing
            return false;
        }

        return !collDoc[CollectionType::noBalance()].trueValue();
    }

    bool Grid::_inBalancingWindow( const BSONObj& balancerDoc , const boost::posix_time::ptime& now ) {
        // check the 'activeWindow' marker
        // if present, it is an interval during the day when the balancer should be active
        // { start: "08:00" , stop: "19:30" }, strftime format is %H:%M
        BSONElement windowElem = balancerDoc[SettingsType::balancerActiveWindow()];
        if ( windowElem.eoo() ) {
            return true;
        }

        // check if both 'start' and 'stop' are present
        if ( ! windowElem.isABSONObj() ) {
            warning() << "'activeWindow' format is { start: \"hh:mm\" , stop: ... }" << balancerDoc << endl;
            return true;
        }
        BSONObj intervalDoc = windowElem.Obj();
        const string start = intervalDoc["start"].str();
        const string stop = intervalDoc["stop"].str();
        if ( start.empty() || stop.empty() ) {
            warning() << "must specify both start and end of balancing window: " << intervalDoc << endl;
            return true;
        }

        // check that both 'start' and 'stop' are valid time-of-day
        boost::posix_time::ptime startTime, stopTime;
        if ( ! toPointInTime( start , &startTime ) || ! toPointInTime( stop , &stopTime ) ) {
            warning() << "cannot parse active window (use hh:mm 24hs format): " << intervalDoc << endl;
            return true;
        }

        LOG(1).stream() << "_inBalancingWindow: "
                        << " now: " << now
                        << " startTime: " << startTime
                        << " stopTime: " << stopTime;

        // allow balancing if during the activeWindow
        // note that a window may be open during the night
        if ( stopTime > startTime ) {
            if ( ( now >= startTime ) && ( now <= stopTime ) ) {
                return true;
            }
        }
        else if ( startTime > stopTime ) {
            if ( ( now >=startTime ) || ( now <= stopTime ) ) {
                return true;
            }
        }

        return false;
    }

    BSONObj Grid::getConfigSetting( const std::string& name ) const {
        ScopedDbConnection conn(configServer.getPrimary().getConnString(), 30);
        BSONObj result = conn->findOne( SettingsType::ConfigNS,
                                        BSON( SettingsType::key(name) ) );
        conn.done();

        return result;
    }

    Grid grid;
}
