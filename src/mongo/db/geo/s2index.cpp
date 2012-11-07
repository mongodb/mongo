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

    static const string SPHERE_2D_NAME = "s2d";

    class GeoSphere2DType : public IndexType {
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

        GeoSphere2DType(const IndexPlugin *plugin, const IndexSpec *spec,
                        const S2IndexingParams &params)
            : IndexType(plugin, spec), _params(params) {
            int geoFields = 0;
            // Categorize the fields we're indexing and make sure we have a geo field.
            BSONObjIterator i(spec->keyPattern);
            while (i.more()) {
                BSONElement e = i.next();
                if (e.type() == String && SPHERE_2D_NAME == e.valuestr()) {
                    _fields.push_back(IndexedField(IndexedField::GEO, e.fieldName()));
                    ++geoFields;
                } else {
                    _fields.push_back(IndexedField(IndexedField::LITERAL, e.fieldName()));
                }
            }
            uassert(16450, "Expect at least one geo field, spec=" + spec->keyPattern.toString(),
                    geoFields >= 1);
        }

        virtual ~GeoSphere2DType() { }

        void getKeys(const BSONObj& obj, BSONObjSet& keys) const {
            verify(_fields.size() >= 1);

            BSONObjSet keysToAdd;
            // We output keys in the same order as the fields we index.
            for (size_t i = 0; i < _fields.size(); ++i) {
                const IndexedField &field = _fields[i];

                // First, we get the keys that this field adds.  Either they're added literally from
                // the value of the field, or they're transformed if the field is geo.
                BSONElementSet fieldElements;
                obj.getFieldsDotted(field.name, fieldElements);

                BSONObj keysForThisField;
                if (IndexedField::GEO == field.type) {
                    keysForThisField = getGeoKeys(fieldElements);
                } else if (IndexedField::LITERAL == field.type) {
                    keysForThisField = getLiteralKeys(fieldElements);
                } else {
                    verify(0);
                }

                // We expect there to be _spec->_missingField() present in the keys if data is
                // missing.  So, this should be non-empty.
                verify(!keysForThisField.isEmpty());

                // We take the Cartesian product of all of the keys.  This requires that we have
                // some keys to take the Cartesian product with.  If keysToAdd.empty(), we
                // initialize it.  
                if (keysToAdd.empty()) {
                    // This should only happen w/the first field.
                    verify(0 == i);
                    BSONObjIterator newIt(keysForThisField);
                    while (newIt.more()) {
                        BSONObjBuilder b;
                        b.append("", newIt.next().String());
                        keysToAdd.insert(b.obj());
                    }
                    continue;
                }

                BSONObjSet updatedKeysToAdd;
                for (BSONObjSet::const_iterator it = keysToAdd.begin(); it != keysToAdd.end();
                     ++it) {

                    BSONObjIterator newIt(keysForThisField);
                    while (newIt.more()) {
                        BSONObjBuilder b;
                        b.appendElements(*it);
                        b.append("", newIt.next().String());
                        updatedKeysToAdd.insert(b.obj());
                    }
                }
                keysToAdd = updatedKeysToAdd;
            }

            if (keysToAdd.size() > _params.maxKeysPerInsert) {
                warning() << "insert of geo object generated lots of keys.\n"
                          << "consider creating larger buckets. obj="
                          << obj;
            }

            for (BSONObjSet::const_iterator it = keysToAdd.begin(); it != keysToAdd.end(); ++it) {
                keys.insert(*it);
            }
        }

        // Entry point for a search.
        virtual shared_ptr<Cursor> newCursor(const BSONObj& query, const BSONObj& order,
                                             int numWanted) const {
            // I copied this from 2d.cpp.  Guard against perversion.
            if (numWanted < 0) numWanted *= -1;
            if (0 == numWanted) numWanted = INT_MAX;

            vector<GeoQueryField> regions;

            // Go through the fields that we index, and for each geo one, make a GeoQueryField
            // object for the S2Cursor class to do intersection testing/cover generating with.
            for (size_t i = 0; i < _fields.size(); ++i) {
                const IndexedField &field = _fields[i];
                if (IndexedField::GEO != field.type) {  continue; }

                // Example of what we're trying to parse:
                // pointA = { "type" : "Point", "coordinates": [ 40, 5 ] }
                // t.find({ "geo" : { "$intersect" : { "$point" : pointA} } })
                // where field.name is "geo"
                BSONElement e = query.getFieldDotted(field.name);
                if (e.eoo()) { continue; }

                if (!e.isABSONObj()) { continue; }
                e = e.embeddedObject().firstElement();

                if (!e.isABSONObj()) { continue; }

                BSONObj::MatchType matchType = static_cast<BSONObj::MatchType>(e.getGtLtOp());
                if (BSONObj::opINTERSECT != matchType && BSONObj::opWITHIN != matchType) {
                    continue;
                }

                e = e.embeddedObject().firstElement();
                if (!e.isABSONObj()) { continue; }

                BSONObj shapeObj = e.embeddedObject();
                if (2 != shapeObj.nFields()) { continue; }

                GeoQueryField geoQueryField(field.name);

                if (strcmp(e.fieldName(), "$geometry")) { continue; }
                if (GeoJSONParser::isPolygon(shapeObj)) {
                    // We can't really pass these things around willy-nilly except by ptr.
                    // The cursor owns them.
                    geoQueryField.polygon = new S2Polygon();
                    GeoJSONParser::parsePolygon(shapeObj, geoQueryField.polygon);
                } else if (GeoJSONParser::isPoint(shapeObj)) { 
                    geoQueryField.cell = new S2Cell();
                    GeoJSONParser::parsePoint(shapeObj, geoQueryField.cell);
                } else if (GeoJSONParser::isLineString(shapeObj)) {
                    geoQueryField.line = new S2Polyline();
                    GeoJSONParser::parseLineString(shapeObj, geoQueryField.line);
                } else {
                    // Maybe it's unknown geometry, maybe it's garbage.
                    // TODO: alert the user?
                    continue;
                }
                regions.push_back(geoQueryField);
            }

            S2Cursor *cursor = new S2Cursor(keyPattern(), getDetails(), query, regions, _params,
                                            numWanted);
            return shared_ptr<Cursor>(cursor);
        }

        virtual IndexSuitability suitability(const BSONObj& query, const BSONObj& order) const {
            for (size_t i = 0; i < _fields.size(); ++i) {
                const IndexedField &field = _fields[i];
                if (IndexedField::GEO != field.type) { continue; }

                BSONElement e = query.getFieldDotted(field.name);
                if (Object != e.type()) { continue; }
                // we only support opWITHIN and opINTERSECT 
                // getGtLtOp is horribly misnamed and really means get the operation.
                switch (e.embeddedObject().firstElement().getGtLtOp()) {
                    case BSONObj::opWITHIN:
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
        BSONObj getGeoKeys(const BSONElementSet &elements) const {
            BSONArrayBuilder aBuilder;

            S2RegionCoverer coverer;
            _params.configureCoverer(&coverer);

            // See here for GeoJSON format: geojson.org/geojson-spec.html
            for (BSONElementSet::iterator i = elements.begin(); i != elements.end(); ++i) {
                const BSONObj &obj = i->Obj();

                vector<string> cells;
                if (GeoJSONParser::isPolygon(obj)) {
                    S2Polygon shape;
                    GeoJSONParser::parsePolygon(obj, &shape);
                    keysFromRegion(&coverer, shape, &cells);
                } else if (GeoJSONParser::isLineString(obj)) {
                    S2Polyline shape;
                    GeoJSONParser::parseLineString(obj, &shape);
                    keysFromRegion(&coverer, shape, &cells);
                } else if (GeoJSONParser::isPoint(obj)) {
                    S2Cell point;
                    GeoJSONParser::parsePoint(obj, &point);
                    keysFromRegion(&coverer, point, &cells);
                } else {
                    // TODO(hk): report an error?
                }

                for (vector<string>::const_iterator it = cells.begin(); it != cells.end(); ++it) {
                    aBuilder.append(*it);
                }
            }

            if (0 == aBuilder.arrSize()) {
                // TODO(hk): We use "" for empty.  I should verify this actually works.
                aBuilder.append("");
            }

            return aBuilder.arr();
        }

        // elements is a non-geo field.  Add the values literally, expanding arrays.
        BSONObj getLiteralKeys(const BSONElementSet &elements) const {
            BSONArrayBuilder builder;
            if (0 == elements.size()) {
                builder.append("");
            } else {
                for (BSONElementSet::iterator i = elements.begin(); i != elements.end(); ++i) {
                    builder.append(*i);
                }
            }
            return builder.arr();
        }

        vector<IndexedField> _fields;
        S2IndexingParams _params;
    };

    class GeoSphere2DIndexPlugin : public IndexPlugin {
    public:
        GeoSphere2DIndexPlugin() : IndexPlugin(SPHERE_2D_NAME) { }

        virtual IndexType* generate(const IndexSpec* spec) const {
            S2IndexingParams params;
            // TODO: parse params optionally from spec?
            params.maxKeysPerInsert = 100;
            // This is advisory.
            params.maxCellsInCovering = 5;
            const double radiusOfEarthInMeters = 6378.1 * 1000.0;
            // These are not advisory.
            params.finestIndexedLevel = S2::kAvgEdge.GetClosestLevel(10.0 / radiusOfEarthInMeters);
            params.coarsestIndexedLevel =
                S2::kAvgEdge.GetClosestLevel(1000000.0 / radiusOfEarthInMeters);
            return new GeoSphere2DType(this, spec, params);
        }
    } geoSphere2DIndexPlugin;
}  // namespace mongo
