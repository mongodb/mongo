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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/geo/geoquery.h"
#include "mongo/db/geo/s2common.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query/get_runner.h"
#include "mongo/db/query/type_explain.h"
#include "mongo/db/structure/collection.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {

    class Geo2dFindNearCmd : public Command {
    public:
        Geo2dFindNearCmd() : Command("geoNear") {}

        virtual LockType locktype() const { return READ; }
        bool slaveOk() const { return true; }
        bool slaveOverrideOk() const { return true; }

        void help(stringstream& h) const {
            h << "http://dochub.mongodb.org/core/geo#GeospatialIndexing-geoNearCommand";
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::find);
            out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
        }

        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string ns = dbname + "." + cmdObj.firstElement().valuestr();

            if (!cmdObj["start"].eoo()) {
                errmsg = "using deprecated 'start' argument to geoNear";
                return false;
            }

            Database* db = cc().database();
            if ( !db ) {
                errmsg = "can't find ns";
                return false;
            }

            Collection* collection = db->getCollection( ns );
            if ( !collection ) {
                errmsg = "can't find ns";
                return false;
            }

            IndexCatalog* indexCatalog = collection->getIndexCatalog();

            // cout << "raw cmd " << cmdObj.toString() << endl;

            // We seek to populate this.
            string nearFieldName;
            bool using2DIndex = false;
            if (!getFieldName(collection, indexCatalog, &nearFieldName, &errmsg, &using2DIndex)) {
                return false;
            }

            uassert(17304, "'near' field must be point",
                    !cmdObj["near"].eoo() && cmdObj["near"].isABSONObj()
                    && GeoParser::isPoint(cmdObj["near"].Obj()));

            bool isSpherical = cmdObj["spherical"].trueValue();
            if (!using2DIndex) {
                uassert(17301, "2dsphere index must have spherical: true", isSpherical);
            }

            // Build the $near expression for the query.
            BSONObjBuilder nearBob;
            if (isSpherical) {
                nearBob.append("$nearSphere", cmdObj["near"].Obj());
            }
            else {
                nearBob.append("$near", cmdObj["near"].Obj());
            }

            if (!cmdObj["maxDistance"].eoo()) {
                uassert(17299, "maxDistance must be a number",cmdObj["maxDistance"].isNumber());
                nearBob.append("$maxDistance", cmdObj["maxDistance"].number());
            }

            if (!cmdObj["minDistance"].eoo()) {
                uassert(17298, "minDistance doesn't work on 2d index", !using2DIndex);
                uassert(17300, "minDistance must be a number",cmdObj["minDistance"].isNumber());
                nearBob.append("$minDistance", cmdObj["minDistance"].number());
            }

            if (!cmdObj["uniqueDocs"].eoo()) {
                nearBob.append("$uniqueDocs", cmdObj["uniqueDocs"].trueValue());
            }

            // And, build the full query expression.
            BSONObjBuilder queryBob;
            queryBob.append(nearFieldName, nearBob.obj());
            if (!cmdObj["query"].eoo() && cmdObj["query"].isABSONObj()) {
                queryBob.appendElements(cmdObj["query"].Obj());
            }
            BSONObj rewritten = queryBob.obj();

            // cout << "rewritten query: " << rewritten.toString() << endl;

            int numWanted = 100;
            const char* limitName = !cmdObj["num"].eoo() ? "num" : "limit";
            BSONElement eNumWanted = cmdObj[limitName];
            if (!eNumWanted.eoo()) {
                uassert(17303, "limit must be number", eNumWanted.isNumber());
                numWanted = eNumWanted.numberInt();
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

            BSONObj projObj = BSON("$pt" << BSON("$meta" << LiteParsedQuery::metaGeoNearPoint) <<
                                   "$dis" << BSON("$meta" << LiteParsedQuery::metaGeoNearDistance));

            CanonicalQuery* cq;
            if (!CanonicalQuery::canonicalize(ns, rewritten, BSONObj(), projObj, 0, numWanted, BSONObj(), &cq).isOK()) {
                errmsg = "Can't parse filter / create query";
                return false;
            }

            Runner* rawRunner;
            if (!getRunner(cq, &rawRunner, 0).isOK()) {
                errmsg = "can't get query runner";
                return false;
            }

            auto_ptr<Runner> runner(rawRunner);

            double totalDistance = 0;
            BSONObjBuilder resultBuilder(result.subarrayStart("results"));
            double farthestDist = 0;

            BSONObj currObj;
            int results = 0;
            while ((results < numWanted) && Runner::RUNNER_ADVANCED == runner->getNext(&currObj, NULL)) {
                // cout << "result is " << currObj.toString() << endl;

                double dist = currObj["$dis"].number() * distanceMultiplier;
                // cout << std::setprecision(10) << "HK GEON mul'd dist is " << dist << " raw dist is " << currObj["$dis"].number() << endl;
                totalDistance += dist;
                if (dist > farthestDist) { farthestDist = dist; }

                BSONObjBuilder oneResultBuilder(
                    resultBuilder.subobjStart(BSONObjBuilder::numStr(results)));
                oneResultBuilder.append("dis", dist);
                if (includeLocs) {
                    oneResultBuilder.appendAs(currObj["$pt"], "loc");
                }

                // strip out '$dis' and '$pt' and the rest gets added as 'obj'.
                BSONObjIterator resIt(currObj);
                BSONObjBuilder resBob;
                while (resIt.more()) {
                    BSONElement elt = resIt.next();
                    if (!mongoutils::str::equals("$pt", elt.fieldName())
                        && !mongoutils::str::equals("$dis", elt.fieldName())) {
                        resBob.append(elt);
                    }
                }
                oneResultBuilder.append("obj", resBob.obj());
                oneResultBuilder.done();
                ++results;
            }

            resultBuilder.done();

            // Fill out the stats subobj.
            BSONObjBuilder stats(result.subobjStart("stats"));

            // Fill in nscanned from the explain.
            TypeExplain* bareExplain;
            Status res = runner->getExplainPlan(&bareExplain);
            if (res.isOK()) {
                auto_ptr<TypeExplain> explain(bareExplain);
                stats.append("nscanned", explain->getNScanned());
                stats.append("objectsLoaded", explain->getNScannedObjects());
            }

            stats.append("avgDistance", totalDistance / results);
            stats.append("maxDistance", farthestDist);
            stats.append("time", cc().curop()->elapsedMillis());
            stats.done();

            return true;
        }

    private:
        bool getFieldName(Collection* collection, IndexCatalog* indexCatalog, string* fieldOut,
                          string* errOut, bool *isFrom2D) {
            vector<IndexDescriptor*> idxs;

            // First, try 2d.
            collection->getIndexCatalog()->findIndexByType(IndexNames::GEO_2D, idxs);
            if (idxs.size() > 1) {
                *errOut = "more than one 2d index, not sure which to run geoNear on";
                return false;
            }

            if (1 == idxs.size()) {
                BSONObj indexKp = idxs[0]->keyPattern();
                BSONObjIterator kpIt(indexKp);
                while (kpIt.more()) {
                    BSONElement elt = kpIt.next();
                    if (String == elt.type() && IndexNames::GEO_2D == elt.valuestr()) {
                        *fieldOut = elt.fieldName();
                        *isFrom2D = true;
                        return true;
                    }
                }
            }

            // Next, 2dsphere.
            idxs.clear();
            collection->getIndexCatalog()->findIndexByType(IndexNames::GEO_2DSPHERE, idxs);
            if (0 == idxs.size()) {
                *errOut = "no geo indices for geoNear";
                return false;
            }

            if (idxs.size() > 1) {
                *errOut = "more than one 2dsphere index, not sure which to run geoNear on";
                return false;
            }

            // 1 == idx.size()
            BSONObj indexKp = idxs[0]->keyPattern();
            BSONObjIterator kpIt(indexKp);
            while (kpIt.more()) {
                BSONElement elt = kpIt.next();
                if (String == elt.type() && IndexNames::GEO_2DSPHERE == elt.valuestr()) {
                    *fieldOut = elt.fieldName();
                    *isFrom2D = false;
                    return true;
                }
            }

            return false;
        }
    } geo2dFindNearCmd;
}  // namespace mongo
