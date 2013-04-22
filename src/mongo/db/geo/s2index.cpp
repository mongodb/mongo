// XXX THIS FILE IS DEPRECATED.  PLEASE DON'T MODIFY.
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
#include "mongo/db/geo/geonear.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/geo/geoquery.h"
#include "mongo/db/index/s2_common.h"
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
                uassert(16700, "Can't parse geometry from element: " + i->toString(),
                        i->isABSONObj());
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

        void getLiteralKeysArray(BSONObj obj, BSONObjSet *out) const {
            BSONObjIterator objIt(obj);
            if (!objIt.more()) {
                // Empty arrays are indexed as undefined.
                BSONObjBuilder b;
                b.appendUndefined("");
                out->insert(b.obj());
            } else {
                // Non-empty arrays are exploded.
                while (objIt.more()) {
                    BSONObjBuilder b;
                    b.appendAs(objIt.next(), "");
                    out->insert(b.obj());
                }
            }
        }

        void getOneLiteralKey(BSONElement elt, BSONObjSet *out) const {
            if (Array == elt.type()) {
                getLiteralKeysArray(elt.Obj(), out);
            } else {
                // One thing, not an array, index as-is.
                BSONObjBuilder b;
                b.appendAs(elt, "");
                out->insert(b.obj());
            }
        }

        // elements is a non-geo field.  Add the values literally, expanding arrays.
        void getLiteralKeys(const BSONElementSet &elements, BSONObjSet *out) const {
            if (0 == elements.size()) {
                // Missing fields are indexed as null.
                BSONObjBuilder b;
                b.appendNull("");
                out->insert(b.obj());
            } else {
                for (BSONElementSet::iterator i = elements.begin(); i != elements.end(); ++i) {
                    getOneLiteralKey(*i, out);
                }
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
            // Near distances are specified in meters...sometimes.
            params.radius = S2IndexingParams::kRadiusOfEarthInMeters;
            // These are not advisory.
            params.finestIndexedLevel = configValueWithDefault(spec, "finestIndexedLevel",
                S2::kAvgEdge.GetClosestLevel(500.0 / params.radius));
            params.coarsestIndexedLevel = configValueWithDefault(spec, "coarsestIndexedLevel",
                S2::kAvgEdge.GetClosestLevel(100 * 1000.0 / params.radius));
            uassert(16687, "coarsestIndexedLevel must be >= 0", params.coarsestIndexedLevel >= 0);
            uassert(16688, "finestIndexedLevel must be <= 30", params.finestIndexedLevel <= 30);
            uassert(16689, "finestIndexedLevel must be >= coarsestIndexedLevel",
                    params.finestIndexedLevel >= params.coarsestIndexedLevel);
            return new S2IndexType(SPHERE_2D_NAME, this, spec, params);
        }

        int configValueWithDefault(const IndexSpec* spec, const string& name, int def) const {
            BSONElement e = spec->info[name];
            if (e.isNumber()) { return e.numberInt(); }
            return def;
        }
    } S2IndexPluginS2D;

}  // namespace mongo
