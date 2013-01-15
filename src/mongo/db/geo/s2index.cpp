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
*/

#include "mongo/db/namespace-inl.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/index.h"
#include "mongo/db/queryutil.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/geo/geoquery.h"
#include "mongo/db/geo/s2common.h"
#include "mongo/db/geo/s2cursor.h"
#include "mongo/db/geo/s2nearcursor.h"
#include "third_party/s2/s2.h"
#include "third_party/s2/s2cell.h"
#include "third_party/s2/s2polygon.h"
#include "third_party/s2/s2polyline.h"
#include "third_party/s2/s2regioncoverer.h"

namespace {
    // Used in a handful of places in GeoSphere2DType below.
    static void keysFromRegion(S2RegionCoverer *coverer, const S2Region &region,
            vector<string> *out) {
        vector<S2CellId> covering;
        coverer->GetCovering(region, &covering);
        for (size_t i = 0; i < covering.size(); ++i) {
            out->push_back(covering[i].toString());
        }
    }
}  // namespace

namespace mongo {
    class S2IndexType : public IndexType {
    public:
        // We keep track of what fields we've indexed and if they're geo or not.
        struct IndexedField {
            enum Type {
                GEO,
                LITERAL
            };

            Type type;
            string name;
            IndexedField(Type t, const string& n) : type(t), name(n) { }
        };

        S2IndexType(const string& geoIdxName, const IndexPlugin *plugin, const IndexSpec *spec,
                    const S2IndexingParams &params) : IndexType(plugin, spec), _params(params) {
            int geoFields = 0;
            // Categorize the fields we're indexing and make sure we have a geo field.
            BSONObjIterator i(spec->keyPattern);
            while (i.more()) {
                BSONElement e = i.next();
                if (e.type() == String && geoIdxName == e.valuestr()) {
                    _fields.push_back(IndexedField(IndexedField::GEO, e.fieldName()));
                    ++geoFields;
                } else {
                    _fields.push_back(IndexedField(IndexedField::LITERAL, e.fieldName()));
                }
            }
            uassert(16450, "Expect at least one geo field, spec=" + spec->keyPattern.toString(),
                    geoFields >= 1);
        }

        virtual ~S2IndexType() { }

        void getKeys(const BSONObj& obj, BSONObjSet& keys) const {
            verify(_fields.size() >= 1);

            BSONObjSet keysToAdd;
            // We output keys in the same order as the fields we index.
            for (size_t i = 0; i < _fields.size(); ++i) {
                const IndexedField &field = _fields[i];

                // First, we get the keys that this field adds.  Either they're added literally from
                // the value of the field, or they're transformed if the field is geo.
                BSONElementSet fieldElements;
                // false means Don't expand the last array, duh.
                obj.getFieldsDotted(field.name, fieldElements, false);

                BSONObjSet keysForThisField;
                if (IndexedField::GEO == field.type) {
                    getGeoKeys(fieldElements, &keysForThisField);
                } else if (IndexedField::LITERAL == field.type) {
                    getLiteralKeys(fieldElements, &keysForThisField);
                } else {
                    verify(0);
                }

                // We expect there to be _spec->_missingField() present in the keys if data is
                // missing.  So, this should be non-empty.
                verify(!keysForThisField.empty());

                // We take the Cartesian product of all of the keys.  This requires that we have
                // some keys to take the Cartesian product with.  If keysToAdd.empty(), we
                // initialize it.  
                if (keysToAdd.empty()) {
                    keysToAdd = keysForThisField;
                    continue;
                }

                BSONObjSet updatedKeysToAdd;
                for (BSONObjSet::const_iterator it = keysToAdd.begin(); it != keysToAdd.end();
                     ++it) {
                    for (BSONObjSet::const_iterator newIt = keysForThisField.begin();
                         newIt!= keysForThisField.end(); ++newIt) {
                        BSONObjBuilder b;
                        b.appendElements(*it);
                        b.append(newIt->firstElement());
                        updatedKeysToAdd.insert(b.obj());
                    }
                }
                keysToAdd = updatedKeysToAdd;
            }

            if (keysToAdd.size() > _params.maxKeysPerInsert) {
                warning() << "insert of geo object generated lots of keys (" << keysToAdd.size()
                          << ") consider creating larger buckets. obj="
                          << obj;
            }

            for (BSONObjSet::const_iterator it = keysToAdd.begin(); it != keysToAdd.end(); ++it) {
                keys.insert(*it);
            }
        }

        // Entry point for a search.
        virtual shared_ptr<Cursor> newCursor(const BSONObj& query, const BSONObj& order,
                                             int numWanted) const {
            vector<GeoQuery> regions;
            bool isNearQuery = false;
            NearQuery nearQuery;

            // Go through the fields that we index, and for each geo one, make
            // a GeoQuery object for the S2*Cursor class to do intersection
            // testing/cover generating with.
            for (size_t i = 0; i < _fields.size(); ++i) {
                const IndexedField &field = _fields[i];
                if (IndexedField::GEO != field.type) { continue; }

                BSONElement e = query.getFieldDotted(field.name);
                if (e.eoo()) { continue; }
                if (!e.isABSONObj()) { continue; }
                BSONObj obj = e.Obj();

                if (nearQuery.parseFrom(obj)) {
                    uassert(16685, "Only one $near clause allowed: " + query.toString(),
                            !isNearQuery);
                    isNearQuery = true;
                    nearQuery.field = field.name;
                    continue;
                }

                GeoQuery geoQueryField(field.name);
                if (!geoQueryField.parseFrom(obj)) {
                    uasserted(16535, "can't parse query (2dsphere): " + obj.toString());
                }
                uassert(16684, "Geometry unsupported: " + obj.toString(),
                        geoQueryField.hasS2Region());
                regions.push_back(geoQueryField);
            }

            // I copied this from 2d.cpp.  Guard against perversion.
            if (numWanted < 0) numWanted *= -1;
            if (0 == numWanted) numWanted = INT_MAX;

            // Remove all the indexed geo regions from the query.  The s2*cursor will
            // instead create a covering for that key to speed up the search.
            //
            // One thing to note is that we create coverings for indexed geo keys during
            // a near search to speed it up further.
            BSONObjBuilder geoFieldsToNuke;
            if (isNearQuery) {
                geoFieldsToNuke.append(nearQuery.field, "");
            }
            for (size_t i = 0; i < regions.size(); ++i) {
                geoFieldsToNuke.append(regions[i].getField(), "");
            }

            // false means we want to filter OUT geoFieldsToNuke, not filter to include only that.
            BSONObj filteredQuery = query.filterFieldsUndotted(geoFieldsToNuke.obj(), false);

            if (isNearQuery) {
                S2NearCursor *cursor = new S2NearCursor(keyPattern(), getDetails(), filteredQuery,
                    nearQuery, regions, _params, numWanted);
                return shared_ptr<Cursor>(cursor);
            } else {
                S2Cursor *cursor = new S2Cursor(keyPattern(), getDetails(), filteredQuery, regions, 
                                                _params, numWanted);
                return shared_ptr<Cursor>(cursor);
            }
        }

        virtual IndexSuitability suitability(const FieldRangeSet& queryConstraints,
                                             const BSONObj& order) const {
            BSONObj query = queryConstraints.originalQuery();

            for (size_t i = 0; i < _fields.size(); ++i) {
                const IndexedField &field = _fields[i];
                if (IndexedField::GEO != field.type) { continue; }

                BSONElement e = query.getFieldDotted(field.name);
                // Some locations are given to us as arrays.  Sigh.
                if (Array == e.type()) { return HELPFUL; }
                if (Object != e.type()) { continue; }
                // getGtLtOp is horribly misnamed and really means get the operation.
                switch (e.embeddedObject().firstElement().getGtLtOp()) {
                    case BSONObj::opNEAR:
                        return OPTIMAL;
                    case BSONObj::opWITHIN: {
                        BSONElement elt = e.embeddedObject().firstElement();
                        if (Object != elt.type()) { continue; }
                        const char* fname = elt.embeddedObject().firstElement().fieldName();
                        if (mongoutils::str::equals("$geometry", fname)
                            || mongoutils::str::equals("$centerSphere", fname)) {
                            return OPTIMAL;
                        } else {
                            return USELESS;
                        }
                    }
                    case BSONObj::opGEO_INTERSECTS:
                        return OPTIMAL;
                    default:
                        return USELESS;
                }
            }
            return USELESS;
        }

        const IndexDetails* getDetails() const { return _spec->getDetails(); }

        // These are used by the geoNear command.  geoNear constructs its own cursor.
        const S2IndexingParams& getParams() const { return _params; }
        void getGeoFieldNames(vector<string> *out) const {
            for (size_t i = 0; i < _fields.size(); ++i) {
                if (IndexedField::GEO == _fields[i].type) {
                    out->push_back(_fields[i].name);
                }
            }
        }
    private:
        // Get the index keys for elements that are GeoJSON.
        void getGeoKeys(const BSONElementSet &elements, BSONObjSet *out) const {
            S2RegionCoverer coverer;
            _params.configureCoverer(&coverer);

            // See here for GeoJSON format: geojson.org/geojson-spec.html
            for (BSONElementSet::iterator i = elements.begin(); i != elements.end(); ++i) {
                if (!i->isABSONObj()) { continue; }  // error?
                const BSONObj &obj = i->Obj();

                vector<string> cells;
                S2Polyline line;
                S2Cell point;
                // We only support GeoJSON polygons.  Why?:
                // 1. we don't automagically do WGS84/flat -> WGS84, and
                // 2. the old polygon format must die.
                if (GeoParser::isGeoJSONPolygon(obj)) {
                    S2Polygon polygon;
                    GeoParser::parseGeoJSONPolygon(obj, &polygon);
                    keysFromRegion(&coverer, polygon, &cells);
                } else if (GeoParser::parseLineString(obj, &line)) {
                    keysFromRegion(&coverer, line, &cells);
                } else if (GeoParser::parsePoint(obj, &point)) {
                    S2CellId parent(point.id().parent(_params.finestIndexedLevel));
                    cells.push_back(parent.toString());
                } else {
                    uasserted(16572, "Can't extract geo keys from object, malformed geometry?:"
                                     + obj.toString());
                }
                uassert(16673, "Unable to generate keys for (likely malformed) geometry: "
                               + obj.toString(),
                        cells.size() > 0);

                for (vector<string>::const_iterator it = cells.begin(); it != cells.end(); ++it) {
                    BSONObjBuilder b;
                    b.append("", *it);
                    out->insert(b.obj());
                }
            }

            if (0 == out->size()) {
                BSONObjBuilder b;
                b.appendNull("");
                out->insert(b.obj());
            }
        }


        // elements is a non-geo field.  Add the values literally, expanding arrays.
        void getLiteralKeys(const BSONElementSet &elements, BSONObjSet *out) const {
            if (0 == elements.size()) {
                BSONObjBuilder b;
                b.appendNull("");
                out->insert(b.obj());
            } else if (1 == elements.size()) {
                BSONObjBuilder b;
                b.appendAs(*elements.begin(), "");
                out->insert(b.obj());
            } else {
                BSONArrayBuilder aBuilder;
                for (BSONElementSet::iterator i = elements.begin(); i != elements.end(); ++i) {
                    aBuilder.append(*i);
                }
                BSONObjBuilder b;
                b.append("", aBuilder.arr());
                out->insert(b.obj());
            }
        }

        vector<IndexedField> _fields;
        S2IndexingParams _params;
    };

    static const string SPHERE_2D_NAME = "2dsphere";
    class S2IndexPlugin : public IndexPlugin {
    public:
        S2IndexPlugin() : IndexPlugin(SPHERE_2D_NAME) { }

        virtual IndexType* generate(const IndexSpec* spec) const {
            S2IndexingParams params;
            params.maxKeysPerInsert = 200;
            // This is advisory.
            params.maxCellsInCovering = 50;
            // Thanks, Wikipedia.
            const double radiusOfEarthInMeters = 6378.1 * 1000.0;
            // We need this to do distance-related things (near queries).
            params.radius = radiusOfEarthInMeters;
            // These are not advisory.
            params.finestIndexedLevel = configValueWithDefault(spec, "finestIndexedLevel",
                S2::kAvgEdge.GetClosestLevel(500.0 / radiusOfEarthInMeters));
            params.coarsestIndexedLevel = configValueWithDefault(spec, "coarsestIndexedLevel",
                S2::kAvgEdge.GetClosestLevel(100 * 1000.0 / radiusOfEarthInMeters));
            uassert(16686, "coarsestIndexedLevel must be >= 0", params.coarsestIndexedLevel >= 0);
            uassert(16687, "finestIndexedLevel must be <= 30", params.finestIndexedLevel <= 30);
            uassert(16688, "finestIndexedLevel must be >= coarsestIndexedLevel",
                    params.finestIndexedLevel >= params.coarsestIndexedLevel);
            return new S2IndexType(SPHERE_2D_NAME, this, spec, params);
        }

        int configValueWithDefault(const IndexSpec* spec, const string& name, int def) const {
            BSONElement e = spec->info[name];
            if (e.isNumber()) { return e.numberInt(); }
            return def;
        }
    } S2IndexPluginS2D;

    bool run2DSphereGeoNear(const IndexDetails &id, BSONObj& cmdObj, string& errmsg,
                            BSONObjBuilder& result) {
        S2IndexType *idxType = static_cast<S2IndexType*>(id.getSpec().getType());
        verify(&id == idxType->getDetails());

        vector<string> geoFieldNames;
        idxType->getGeoFieldNames(&geoFieldNames);

        // NOTE(hk): If we add a new argument to geoNear, we could have a
        // 2dsphere index with multiple indexed geo fields, and the geoNear
        // could pick the one to run over.  Right now, we just require one.
        uassert(16552, "geoNear requiers exactly one indexed geo field", 1 == geoFieldNames.size());
        NearQuery nearQuery(geoFieldNames[0]);
        uassert(16679, "Invalid geometry given as arguments to geoNear: " + cmdObj.toString(),
                nearQuery.parseFromGeoNear(cmdObj));
        uassert(16683, "geoNear on 2dsphere index requires spherical",
                cmdObj["spherical"].trueValue());

        // We support both "num" and "limit" options to control limit
        int numWanted = 100;
        const char* limitName = cmdObj["num"].isNumber() ? "num" : "limit";
        if (cmdObj[limitName].isNumber()) {
            numWanted = cmdObj[limitName].numberInt();
            verify(numWanted >= 0);
        }

        // Add the location information to each result as a field with name 'loc'.
        bool includeLocs = false;
        if (!cmdObj["includeLocs"].eoo()) includeLocs = cmdObj["includeLocs"].trueValue();

        // The non-near query part.
        BSONObj query;
        if (cmdObj["query"].isABSONObj()) {
            query = cmdObj["query"].embeddedObject();
        }

        double distanceMultiplier = 1.0;
        if (cmdObj["distanceMultiplier"].isNumber()) {
            distanceMultiplier = cmdObj["distanceMultiplier"].number();
        }

        // NOTE(hk): For a speedup, we could look through the query to see if
        // we've geo-indexed any of the fields in it.
        vector<GeoQuery> regions;

        scoped_ptr<S2NearCursor> cursor(new S2NearCursor(idxType->keyPattern(),
            idxType->getDetails(), query, nearQuery, regions, idxType->getParams(),
            numWanted));

        double totalDistance = 0;
        int results = 0;
        BSONObjBuilder resultBuilder(result.subarrayStart("results"));
        double farthestDist = 0;

        while (cursor->ok()) {
            double dist = cursor->currentDistance();
            dist *= distanceMultiplier;
            totalDistance += dist;
            if (dist > farthestDist) { farthestDist = dist; }

            BSONObjBuilder oneResultBuilder(
                resultBuilder.subobjStart(BSONObjBuilder::numStr(results)));
            oneResultBuilder.append("dis", dist);
            if (includeLocs) {
                BSONElementSet geoFieldElements;
                cursor->current().getFieldsDotted(geoFieldNames[0], geoFieldElements, false);
                for (BSONElementSet::iterator oi = geoFieldElements.begin();
                        oi != geoFieldElements.end(); ++oi) {
                    if (oi->isABSONObj()) {
                        oneResultBuilder.appendAs(*oi, "loc");
                    }
                }
            }

            oneResultBuilder.append("obj", cursor->current());
            oneResultBuilder.done();
            ++results;
            cursor->advance();
        }

        resultBuilder.done();

        BSONObjBuilder stats(result.subobjStart("stats"));
        stats.append("time", cc().curop()->elapsedMillis());
        stats.appendNumber("nscanned", cursor->nscanned());
        stats.append("avgDistance", totalDistance / results);
        stats.append("maxDistance", farthestDist);
        stats.done();

        return true;
    }
}  // namespace mongo
