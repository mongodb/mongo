/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

class Geo2dFindNearCmd : public BasicCommand {
public:
    Geo2dFindNearCmd() : BasicCommand("geoNear") {}

    std::string help() const override {
        return "The geoNear command is deprecated. See "
               "http://dochub.mongodb.org/core/geoNear-deprecation for more detail on its "
               "replacement.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbname, cmdObj).ns();
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        RARELY {
            warning() << "Support for the geoNear command has been deprecated. Please plan to "
                         "rewrite geoNear commands using the $near query operator, the $nearSphere "
                         "query operator, or the $geoNear aggregation stage. See "
                         "http://dochub.mongodb.org/core/geoNear-deprecation.";
        }

        const NamespaceString nss(parseNs(dbName, cmdObj));

        // We support both "num" and "limit" options to control limit
        long long limit = 100;
        if (cmdObj["num"].isNumber())
            limit = cmdObj["num"].safeNumberLong();
        else if (cmdObj["limit"].isNumber())
            limit = cmdObj["limit"].safeNumberLong();

        const auto query = extractQuery(cmdObj);
        const auto collation = extractCollation(cmdObj);

        const auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

        auto shardResponses = scatterGatherVersionedTargetByRoutingTable(
            opCtx,
            nss.db(),
            nss,
            routingInfo,
            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
            ReadPreferenceSetting::get(opCtx),
            Shard::RetryPolicy::kIdempotent,
            query,
            collation);

        std::multimap<double, BSONObj> results;
        BSONArrayBuilder shardArray;
        std::string nearStr;
        double time = 0;
        double btreelocs = 0;
        double nscanned = 0;
        double objectsLoaded = 0;

        for (const auto& shardResponse : shardResponses) {
            const auto response = uassertStatusOK(shardResponse.swResponse);
            uassertStatusOK(getStatusFromCommandResult(response.data));

            shardArray.append(shardResponse.shardId.toString());
            const auto& shardResult = response.data;

            if (shardResult.hasField("near")) {
                nearStr = shardResult["near"].String();
            }
            time += shardResult["stats"]["time"].Number();
            if (!shardResult["stats"]["btreelocs"].eoo()) {
                btreelocs += shardResult["stats"]["btreelocs"].Number();
            }
            nscanned += shardResult["stats"]["nscanned"].Number();
            if (!shardResult["stats"]["objectsLoaded"].eoo()) {
                objectsLoaded += shardResult["stats"]["objectsLoaded"].Number();
            }

            BSONForEach(obj, shardResult["results"].embeddedObject()) {
                results.insert(
                    std::make_pair(obj["dis"].Number(), obj.embeddedObject().getOwned()));
            }

            // TODO: maybe shrink results if size() > limit
        }

        result.append("ns", nss.ns());
        result.append("near", nearStr);

        long long outCount = 0;
        double totalDistance = 0;
        double maxDistance = 0;
        {
            BSONArrayBuilder sub(result.subarrayStart("results"));
            for (std::multimap<double, BSONObj>::const_iterator it(results.begin()),
                 end(results.end());
                 it != end && outCount < limit;
                 ++it, ++outCount) {
                totalDistance += it->first;
                maxDistance = it->first;  // guaranteed to be highest so far

                sub.append(it->second);
            }
            sub.done();
        }

        {
            BSONObjBuilder sub(result.subobjStart("stats"));
            sub.append("time", time);
            sub.append("btreelocs", btreelocs);
            sub.append("nscanned", nscanned);
            sub.append("objectsLoaded", objectsLoaded);
            sub.append("avgDistance", (outCount == 0) ? 0 : (totalDistance / outCount));
            sub.append("maxDistance", maxDistance);
            sub.append("shards", shardArray.arr());
            sub.done();
        }

        return true;
    }

} geo2dFindNearCmd;

}  // namespace
}  // namespace mongo
