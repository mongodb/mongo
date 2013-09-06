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

#include "mongo/db/geo/geonear.h"

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/geo/s2common.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index/2d_index_cursor.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/s2_near_cursor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/pdfile.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {

    GeoNearArguments::GeoNearArguments(const BSONObj &cmdObj) {
        // If 'num' is passed, use it and ignore 'limit'. Otherwise use 'limit'.
        const char* limitName = !cmdObj["num"].eoo() ? "num" : "limit";
        BSONElement eNumWanted = cmdObj[limitName];
        if (!eNumWanted.eoo()) {
            uassert(17032, str::stream() << limitName << " must be a number",
                    eNumWanted.isNumber());
            numWanted = eNumWanted.numberInt();
        } else {
            numWanted = 100;
        }

        if (!cmdObj["uniqueDocs"].eoo()) {
            uniqueDocs = cmdObj["uniqueDocs"].trueValue();
        } else {
            uniqueDocs = false;
        }

        if (!cmdObj["includeLocs"].eoo()) {
            includeLocs = cmdObj["includeLocs"].trueValue();
        } else {
            includeLocs = false;
        }

        if (cmdObj["query"].isABSONObj()) {
            query = cmdObj["query"].embeddedObject();
        }

        BSONElement eDistanceMultiplier = cmdObj["distanceMultiplier"];
        if (!eDistanceMultiplier.eoo()) {
            uassert(17033, "distanceMultiplier must be a number", eDistanceMultiplier.isNumber());
            distanceMultiplier = eDistanceMultiplier.number();
            uassert(17034, "distanceMultiplier must be non-negative", distanceMultiplier >= 0);
        } else {
            distanceMultiplier = 1.0;
        }

        isSpherical = cmdObj["spherical"].trueValue();
    }

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
            out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
        }

        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string ns = dbname + "." + cmdObj.firstElement().valuestr();
            NamespaceDetails *d = nsdetails(ns);

            if (NULL == d) {
                errmsg = "can't find ns";
                return false;
            }

            GeoNearArguments commonArgs(cmdObj);
            if (commonArgs.numWanted < 0) {
                errmsg = "numWanted must be >= 0";
                return false;
            }

            vector<int> idxs;

            d->findIndexByType(IndexNames::GEO_2D, idxs);
            if (idxs.size() > 1) {
                errmsg = "more than one 2d index, not sure which to run geoNear on";
                return false;
            }

            unordered_map<string, double> statsMap;

            if (1 == idxs.size()) {
                result.append("ns", ns);
                twod_internal::TwoDGeoNearRunner::run2DGeoNear(d, idxs[0], cmdObj, commonArgs,
                                                               errmsg, result, &statsMap);
                BSONObjBuilder stats(result.subobjStart("stats"));
                for (unordered_map<string, double>::const_iterator it = statsMap.begin();
                     it != statsMap.end(); ++it) {
                    stats.append(it->first, it->second);
                }
                stats.append("time", cc().curop()->elapsedMillis());
                stats.done();
                return true;
            }

            d->findIndexByType(IndexNames::GEO_2DSPHERE, idxs);
            if (idxs.size() > 1) {
                errmsg = "more than one 2dsphere index, not sure which to run geoNear on";
                return false;
            }

            if (1 == idxs.size()) {
                result.append("ns", ns);
                run2DSphereGeoNear(d, idxs[0], cmdObj, commonArgs, errmsg, result);
                return true;
            }

            errmsg = "no geo indices for geoNear";
            return false;
        }

    private:

        static bool run2DSphereGeoNear(NamespaceDetails* nsDetails, int idxNo, BSONObj& cmdObj,
                                       const GeoNearArguments &parsedArgs, string& errmsg,
                                       BSONObjBuilder& result) {
            auto_ptr<IndexDescriptor> descriptor(CatalogHack::getDescriptor(nsDetails, idxNo));
            auto_ptr<S2AccessMethod> sam(new S2AccessMethod(descriptor.get()));
            const S2IndexingParams& params = sam->getParams();
            auto_ptr<S2NearIndexCursor> nic(new S2NearIndexCursor(descriptor.get(), params));

            vector<string> geoFieldNames;
            BSONObjIterator i(descriptor->keyPattern());
            while (i.more()) {
                BSONElement e = i.next();
                if (e.type() == String && IndexNames::GEO_2DSPHERE == e.valuestr()) {
                    geoFieldNames.push_back(e.fieldName());
                }
            }

            // NOTE(hk): If we add a new argument to geoNear, we could have a
            // 2dsphere index with multiple indexed geo fields, and the geoNear
            // could pick the one to run over.  Right now, we just require one.
            uassert(16552, "geoNear requires exactly one indexed geo field", 1 == geoFieldNames.size());
            NearQuery nearQuery(geoFieldNames[0]);
            uassert(16679, "Invalid geometry given as arguments to geoNear: " + cmdObj.toString(),
                    nearQuery.parseFromGeoNear(cmdObj, params.radius));
            uassert(16683, "geoNear on 2dsphere index requires spherical",
                    parsedArgs.isSpherical);

            // NOTE(hk): For a speedup, we could look through the query to see if
            // we've geo-indexed any of the fields in it.
            vector<GeoQuery> regions;

            nic->seek(parsedArgs.query, nearQuery, regions);

            // We do pass in the query above, but it's just so we can possibly use it in our index
            // scan.  We have to do our own matching.
            auto_ptr<Matcher> matcher(new Matcher(parsedArgs.query));

            double totalDistance = 0;
            BSONObjBuilder resultBuilder(result.subarrayStart("results"));
            double farthestDist = 0;

            int results;
            for (results = 0; results < parsedArgs.numWanted && !nic->isEOF(); ++results) {
                BSONObj currObj = nic->getValue().obj();
                if (!matcher->matches(currObj)) {
                    --results;
                    nic->next();
                    continue;
                }

                double dist = nic->currentDistance();
                // If we got the distance in radians, output it in radians too.
                if (nearQuery.fromRadians) { dist /= params.radius; }
                dist *= parsedArgs.distanceMultiplier;
                totalDistance += dist;
                if (dist > farthestDist) { farthestDist = dist; }

                BSONObjBuilder oneResultBuilder(
                    resultBuilder.subobjStart(BSONObjBuilder::numStr(results)));
                oneResultBuilder.append("dis", dist);
                if (parsedArgs.includeLocs) {
                    BSONElementSet geoFieldElements;
                    currObj.getFieldsDotted(geoFieldNames[0], geoFieldElements, false);
                    for (BSONElementSet::iterator oi = geoFieldElements.begin();
                            oi != geoFieldElements.end(); ++oi) {
                        if (oi->isABSONObj()) {
                            oneResultBuilder.appendAs(*oi, "loc");
                        }
                    }
                }

                oneResultBuilder.append("obj", currObj);
                oneResultBuilder.done();
                nic->next();
            }

            resultBuilder.done();

            BSONObjBuilder stats(result.subobjStart("stats"));
            stats.appendNumber("nscanned", nic->nscanned());
            stats.append("avgDistance", totalDistance / results);
            stats.append("maxDistance", farthestDist);
            stats.append("time", cc().curop()->elapsedMillis());
            stats.done();

            return true;
        }
    } geo2dFindNearCmd;
}  // namespace mongo
