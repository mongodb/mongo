/**
*    Copyright (C) 2012-2014 MongoDB Inc.
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

#include <vector>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/util/log.h"

namespace mongo {

using std::unique_ptr;
using std::stringstream;

/**
 * The geoNear command is deprecated. Users should prefer the $near query operator, the $nearSphere
 * query operator, or the $geoNear aggregation stage. See
 * http://dochub.mongodb.org/core/geoNear-deprecation for more detail.
 */
class Geo2dFindNearCmd : public ErrmsgCommandDeprecated {
public:
    Geo2dFindNearCmd() : ErrmsgCommandDeprecated("geoNear") {}

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool supportsReadConcern(const std::string& dbName,
                             const BSONObj& cmdObj,
                             repl::ReadConcernLevel level) const final {
        return true;
    }

    ReadWriteType getReadWriteType() const {
        return ReadWriteType::kRead;
    }

    std::size_t reserveBytesForReply() const override {
        return FindCommon::kInitReplyBufferSize;
    }

    std::string help() const override {
        return "http://dochub.mongodb.org/core/geo#GeospatialIndexing-geoNearCommand";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::find);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool errmsgRun(OperationContext* opCtx,
                   const string& dbname,
                   const BSONObj& cmdObj,
                   string& errmsg,
                   BSONObjBuilder& result) {
        // Do not log the deprecation warning when in a direct client, since the $geoNear
        // aggregation stage runs the geoNear command in a direct client.
        RARELY if (!opCtx->getClient()->isInDirectClient()) {
            warning() << "Support for the geoNear command has been deprecated. Please plan to "
                         "rewrite geoNear commands using the $near query operator, the $nearSphere "
                         "query operator, or the $geoNear aggregation stage. See "
                         "http://dochub.mongodb.org/core/geoNear-deprecation.";
        }

        if (!cmdObj["start"].eoo()) {
            errmsg = "using deprecated 'start' argument to geoNear";
            return false;
        }

        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbname, cmdObj));
        AutoGetCollectionForReadCommand ctx(opCtx, nss);

        Collection* collection = ctx.getCollection();
        if (!collection) {
            errmsg = "can't find ns";
            return false;
        }

        auto nearFieldName = getFieldName(opCtx, collection, cmdObj);

        PointWithCRS point;
        uassert(17304,
                "'near' field must be point",
                GeoParser::parseQueryPoint(cmdObj["near"], &point).isOK());

        bool isSpherical = cmdObj["spherical"].trueValue();

        // Build the $near expression for the query.
        BSONObjBuilder nearBob;
        if (isSpherical) {
            nearBob.append("$nearSphere", cmdObj["near"].Obj());
        } else {
            nearBob.append("$near", cmdObj["near"].Obj());
        }

        if (!cmdObj["maxDistance"].eoo()) {
            uassert(17299, "maxDistance must be a number", cmdObj["maxDistance"].isNumber());
            nearBob.append("$maxDistance", cmdObj["maxDistance"].number());
        }

        if (!cmdObj["minDistance"].eoo()) {
            uassert(17300, "minDistance must be a number", cmdObj["minDistance"].isNumber());
            nearBob.append("$minDistance", cmdObj["minDistance"].number());
        }

        if (!cmdObj["uniqueDocs"].eoo()) {
            warning() << nss << ": ignoring deprecated uniqueDocs option in geoNear command";
        }

        // And, build the full query expression.
        BSONObjBuilder queryBob;
        queryBob.append(nearFieldName, nearBob.obj());
        if (!cmdObj["query"].eoo() && cmdObj["query"].isABSONObj()) {
            queryBob.appendElements(cmdObj["query"].Obj());
        }
        BSONObj rewritten = queryBob.obj();

        // Extract the collation, if it exists.
        BSONObj collation;
        {
            BSONElement collationElt;
            Status collationEltStatus =
                bsonExtractTypedField(cmdObj, "collation", BSONType::Object, &collationElt);
            if (!collationEltStatus.isOK() && (collationEltStatus != ErrorCodes::NoSuchKey)) {
                uassertStatusOK(collationEltStatus);
            }
            if (collationEltStatus.isOK()) {
                collation = collationElt.Obj();
            }
        }

        long long numWanted = 100;
        const char* limitName = !cmdObj["num"].eoo() ? "num" : "limit";
        BSONElement eNumWanted = cmdObj[limitName];
        if (!eNumWanted.eoo()) {
            uassert(17303, "limit must be number", eNumWanted.isNumber());
            numWanted = eNumWanted.safeNumberLong();
            uassert(17302, "limit must be >=0", numWanted >= 0);
        }

        bool includeLocs = false;
        if (!cmdObj["includeLocs"].eoo()) {
            includeLocs = cmdObj["includeLocs"].trueValue();
        }

        double distanceMultiplier = 1.0;
        BSONElement eDistanceMultiplier = cmdObj["distanceMultiplier"];
        if (!eDistanceMultiplier.eoo()) {
            uassert(17296, "distanceMultiplier must be a number", eDistanceMultiplier.isNumber());
            distanceMultiplier = eDistanceMultiplier.number();
            uassert(17297, "distanceMultiplier must be non-negative", distanceMultiplier >= 0);
        }

        BSONObj projObj = BSON("$pt" << BSON("$meta" << QueryRequest::metaGeoNearPoint) << "$dis"
                                     << BSON("$meta" << QueryRequest::metaGeoNearDistance));

        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(rewritten);
        qr->setProj(projObj);
        qr->setLimit(numWanted);
        qr->setCollation(collation);
        const ExtensionsCallbackReal extensionsCallback(opCtx, &nss);
        const boost::intrusive_ptr<ExpressionContext> expCtx;
        auto statusWithCQ =
            CanonicalQuery::canonicalize(opCtx,
                                         std::move(qr),
                                         expCtx,
                                         extensionsCallback,
                                         MatchExpressionParser::kAllowAllSpecialFeatures);
        if (!statusWithCQ.isOK()) {
            errmsg = "Can't parse filter / create query";
            return false;
        }
        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        // Prevent chunks from being cleaned up during yields - this allows us to only check the
        // version on initial entry into geoNear.
        auto rangePreserver = CollectionShardingState::get(opCtx, nss)->getMetadata(opCtx);

        const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
        const PlanExecutor::YieldPolicy yieldPolicy =
            readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern
            ? PlanExecutor::INTERRUPT_ONLY
            : PlanExecutor::YIELD_AUTO;
        auto exec = uassertStatusOK(getExecutor(opCtx, collection, std::move(cq), yieldPolicy, 0));

        auto curOp = CurOp::get(opCtx);
        {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curOp->setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
        }

        double totalDistance = 0;
        BSONObjBuilder resultBuilder(result.subarrayStart("results"));
        double farthestDist = 0;

        BSONObj currObj;
        long long results = 0;
        PlanExecutor::ExecState state;
        while (PlanExecutor::ADVANCED == (state = exec->getNext(&currObj, NULL))) {
            // Come up with the correct distance.
            double dist = currObj["$dis"].number() * distanceMultiplier;
            totalDistance += dist;
            if (dist > farthestDist) {
                farthestDist = dist;
            }

            // Strip out '$dis' and '$pt' from the result obj.  The rest gets added as 'obj'
            // in the command result.
            BSONObjIterator resIt(currObj);
            BSONObjBuilder resBob;
            while (resIt.more()) {
                BSONElement elt = resIt.next();
                if (!mongoutils::str::equals("$pt", elt.fieldName()) &&
                    !mongoutils::str::equals("$dis", elt.fieldName())) {
                    resBob.append(elt);
                }
            }
            BSONObj resObj = resBob.obj();

            // Don't make a too-big result object.
            if (resultBuilder.len() + resObj.objsize() > BSONObjMaxUserSize) {
                warning() << "Too many geoNear results for query " << redact(rewritten)
                          << ", truncating output.";
                break;
            }

            // Add the next result to the result builder.
            BSONObjBuilder oneResultBuilder(
                resultBuilder.subobjStart(BSONObjBuilder::numStr(results)));
            oneResultBuilder.append("dis", dist);
            if (includeLocs) {
                oneResultBuilder.appendAs(currObj["$pt"], "loc");
            }
            oneResultBuilder.append("obj", resObj);
            oneResultBuilder.done();

            ++results;

            // Break if we have the number of requested result documents.
            if (results >= numWanted) {
                break;
            }
        }

        resultBuilder.done();

        // Return an error if execution fails for any reason.
        if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
            log() << "Plan executor error during geoNear command: " << PlanExecutor::statestr(state)
                  << ", stats: " << redact(Explain::getWinningPlanStats(exec.get()));

            uassertStatusOK(WorkingSetCommon::getMemberObjectStatus(currObj).withContext(
                "Executor error during geoNear command"));
        }

        PlanSummaryStats summary;
        Explain::getSummaryStats(*exec, &summary);

        // Fill out the stats subobj.
        BSONObjBuilder stats(result.subobjStart("stats"));

        stats.appendNumber("nscanned", summary.totalKeysExamined);
        stats.appendNumber("objectsLoaded", summary.totalDocsExamined);

        if (results > 0) {
            stats.append("avgDistance", totalDistance / results);
        }
        stats.append("maxDistance", farthestDist);
        stats.appendIntOrLL("time",
                            durationCount<Microseconds>(curOp->elapsedTimeExcludingPauses()));
        stats.done();

        collection->infoCache()->notifyOfQuery(opCtx, summary.indexesUsed);

        curOp->debug().setPlanSummaryMetrics(summary);

        if (curOp->shouldDBProfile()) {
            BSONObjBuilder execStatsBob;
            Explain::getWinningPlanStats(exec.get(), &execStatsBob);
            curOp->debug().execStats = execStatsBob.obj();
        }

        return true;
    }

private:
    /**
     * Given a collection and the geoNear command parameters, returns the field path over which
     * the geoNear should operate.
     *
     * Throws an assertion with ErrorCodes::IndexNotFound if there is no single geo index
     * which this geoNear command should use.
     */
    StringData getFieldName(OperationContext* opCtx, Collection* collection, BSONObj cmdObj) {
        if (auto keyElt = cmdObj[DocumentSourceGeoNear::kKeyFieldName]) {
            uassert(ErrorCodes::TypeMismatch,
                    str::stream() << "geoNear parameter '" << DocumentSourceGeoNear::kKeyFieldName
                                  << "' must be of type string but found type: "
                                  << typeName(keyElt.type()),
                    keyElt.type() == BSONType::String);
            auto fieldName = keyElt.valueStringData();
            uassert(ErrorCodes::BadValue,
                    str::stream() << "$geoNear parameter '" << DocumentSourceGeoNear::kKeyFieldName
                                  << "' cannot be the empty string",
                    !fieldName.empty());
            return fieldName;
        }

        vector<IndexDescriptor*> idxs;

        // First, try 2d.
        collection->getIndexCatalog()->findIndexByType(opCtx, IndexNames::GEO_2D, idxs);
        uassert(ErrorCodes::IndexNotFound,
                "more than one 2d index, not sure which to run geoNear on",
                idxs.size() <= 1u);

        if (1 == idxs.size()) {
            BSONObj indexKp = idxs[0]->keyPattern();
            BSONObjIterator kpIt(indexKp);
            while (kpIt.more()) {
                BSONElement elt = kpIt.next();
                if (BSONType::String == elt.type() && IndexNames::GEO_2D == elt.valuestr()) {
                    return elt.fieldNameStringData();
                }
            }
        }

        // Next, 2dsphere.
        idxs.clear();
        collection->getIndexCatalog()->findIndexByType(opCtx, IndexNames::GEO_2DSPHERE, idxs);
        uassert(ErrorCodes::IndexNotFound, "no geo indices for geoNear", !idxs.empty());
        uassert(ErrorCodes::IndexNotFound,
                "more than one 2dsphere index, not sure which to run geoNear on",
                idxs.size() == 1u);

        // 1 == idx.size().
        BSONObj indexKp = idxs[0]->keyPattern();
        BSONObjIterator kpIt(indexKp);
        while (kpIt.more()) {
            BSONElement elt = kpIt.next();
            if (BSONType::String == elt.type() && IndexNames::GEO_2DSPHERE == elt.valuestr()) {
                return elt.fieldNameStringData();
            }
        }

        MONGO_UNREACHABLE;
    }
} geo2dFindNearCmd;
}  // namespace mongo
