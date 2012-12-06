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
#include "mongo/db/jsobj.h"
#include "mongo/db/index.h"
#include "mongo/db/queryutil.h"
#include "mongo/db/geo/geojsonparser.h"
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

        bool parseLegacy(const BSONObj &obj, QueryGeometry *out, bool *isNear, bool *intersect,
                         double *maxDistance) const {
            // Legacy intersect parsing: t.find({ loc : [0,0] })
            if (out->parseFrom(obj)) {
                *isNear = true;
                return true;
            }

            bool ret = false;
            BSONObjIterator it(obj);
            while (it.more()) {
                BSONElement e = it.next();
                // Legacy near parsing: t.find({ loc : { $near: [0,0], $maxDistance: 3 }})
                // Legacy near parsing: t.find({ loc : { $near: [0,0] }})
                if (mongoutils::str::equals(e.fieldName(), "$near")) {
                    if (out->parseFrom(e.embeddedObject())) {
                        *isNear = true;
                        ret = true;
                    }
                } else if (mongoutils::str::equals(e.fieldName(), "$maxDistance")) {
                    *maxDistance = e.Number();
                }
            }
            return ret;
        }

        bool parseQuery(const BSONObj &obj, QueryGeometry *out, bool *isNear, bool *intersect,
                        double *maxDistance) const {
            // pointA = { "type" : "Point", "coordinates": [ 40, 5 ] }
            // t.find({ "geo" : { "$intersect" : { "$geometry" : pointA} } })
            // t.find({ "geo" : { "$near" : { "$geometry" : pointA, $maxDistance : 20 }}})
            // where field.name is "geo"
            BSONElement e = obj.firstElement();
            if (!e.isABSONObj()) { return false; }

            BSONObj::MatchType matchType = static_cast<BSONObj::MatchType>(e.getGtLtOp());
            if (BSONObj::opINTERSECT == matchType) {
                *intersect = true;
            } else if (BSONObj::opNEAR == matchType) {
                *isNear = true;
            } else {
                return false;
            }

            bool ret = false;
            BSONObjIterator argIt(e.embeddedObject());
            while (argIt.more()) {
                BSONElement e = argIt.next();
                if (mongoutils::str::equals(e.fieldName(), "$geometry")) {
                    if (e.isABSONObj() && out->parseFrom(e.Obj())) {
                        ret = true;
                    }
                } else if (mongoutils::str::equals(e.fieldName(), "$maxDistance")) {
                    if (e.isNumber()) {
                        *maxDistance = e.Number();
                    }
                }
            }
            return ret;
        }

        // Entry point for a search.
        virtual shared_ptr<Cursor> newCursor(const BSONObj& query, const BSONObj& order,
                                             int numWanted) const {
            vector<QueryGeometry> regions;
            double maxDistance = DBL_MAX;
            bool isNear = false;
            bool isIntersect = false;

            // Go through the fields that we index, and for each geo one, make a QueryGeometry
            // object for the S2Cursor class to do intersection testing/cover generating with.
            for (size_t i = 0; i < _fields.size(); ++i) {
                const IndexedField &field = _fields[i];
                if (IndexedField::GEO != field.type) { continue; }

                BSONElement e = query.getFieldDotted(field.name);
                if (e.eoo()) { continue; }
                if (!e.isABSONObj()) { continue; }
                BSONObj obj = e.Obj();

                QueryGeometry geoQueryField(field.name);
                if (parseLegacy(obj, &geoQueryField, &isNear, &isIntersect, &maxDistance)) {
                    regions.push_back(geoQueryField);
                } else if (parseQuery(obj, &geoQueryField, &isNear, &isIntersect, &maxDistance)) {
                    regions.push_back(geoQueryField);
                } else {
                    uasserted(16535, "can't parse query for *2d geo search: " + obj.toString());
                }
            }

            if (isNear && isIntersect ) {
                for (size_t j = 0; j < regions.size(); ++j) { regions[j].free(); }
                uasserted(16474, "Can't do both near and intersect, query: " +  query.toString());
            }

            // I copied this from 2d.cpp.  Guard against perversion.
            if (numWanted < 0) numWanted *= -1;
            if (0 == numWanted) numWanted = INT_MAX;

            if (isNear) {
                S2NearCursor *cursor = new S2NearCursor(keyPattern(), getDetails(), query, regions,
                                                        _params, numWanted, maxDistance);
                return shared_ptr<Cursor>(cursor);
            } else {
                // Default to intersect.
                S2Cursor *cursor = new S2Cursor(keyPattern(), getDetails(), query, regions, _params,
                                                numWanted);
                return shared_ptr<Cursor>(cursor);
            }
        }

        virtual IndexSuitability suitability(const BSONObj& query, const BSONObj& order) const {
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
                    case BSONObj::opINTERSECT:
                        return OPTIMAL;
                    default:
                        return HELPFUL;
                }
            }
            return USELESS;
        }

        const IndexDetails* getDetails() const { return _spec->getDetails(); }
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
                S2Polygon polygon;
                S2Polyline line;
                S2Cell point;
                if (GeoJSONParser::parsePolygon(obj, &polygon)) {
                    keysFromRegion(&coverer, polygon, &cells);
                } else if (GeoJSONParser::parseLineString(obj, &line)) {
                    keysFromRegion(&coverer, line, &cells);
                } else if (GeoJSONParser::parsePoint(obj, &point)) {
                    keysFromRegion(&coverer, point, &cells);
                } else {
                    warning() << "unknown geometry: " << i->toString();
                }

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
            params.finestIndexedLevel = S2::kAvgEdge.GetClosestLevel(100.0 / radiusOfEarthInMeters);
            params.coarsestIndexedLevel = 
                S2::kAvgEdge.GetClosestLevel(100 * 1000.0 / radiusOfEarthInMeters);
            return new S2IndexType(SPHERE_2D_NAME, this, spec, params);
        }
    } S2IndexPluginS2D;
}  // namespace mongo
