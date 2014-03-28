/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/index/s2_access_method.h"

#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/geo/geoquery.h"
#include "mongo/db/geo/s2.h"
#include "mongo/db/geo/s2common.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/s2_key_generator.h"
#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"

#include "third_party/s2/s2cell.h"
#include "third_party/s2/s2regioncoverer.h"

namespace mongo {

    S2KeyGenerator::S2KeyGenerator( const BSONObj& keyPattern, const S2IndexingParams& params )
        : _keyPattern( keyPattern.getOwned() ), _params( params ) {
    }

    //
    // Helper functions for getS2Keys
    //

    static void S2KeysFromRegion(S2RegionCoverer *coverer, const S2Region &region,
                               vector<string> *out) {
        vector<S2CellId> covering;
        coverer->GetCovering(region, &covering);
        for (size_t i = 0; i < covering.size(); ++i) {
            out->push_back(covering[i].toString());
        }
    }

    bool S2GetKeysForObject(const BSONObj& obj,
                            const S2IndexingParams& params,
                            vector<string>* out) {
        S2RegionCoverer coverer;
        params.configureCoverer(&coverer);

        GeometryContainer geoContainer;
        if (!geoContainer.parseFrom(obj)) { return false; }

        // Only certain geometries can be indexed in the old index format S2_INDEX_VERSION_1.  See
        // definition of S2IndexVersion for details.
        if (params.indexVersion == S2_INDEX_VERSION_1 && !geoContainer.isSimpleContainer()) {
            return false;
        }

        if (!geoContainer.hasS2Region()) { return false; }

        S2KeysFromRegion(&coverer, geoContainer.getRegion(), out);

        return true;
    }

    /**
     *  Get the index keys for elements that are GeoJSON.
     *  Used by getS2Keys.
     */
    void getS2GeoKeys(const BSONObj& document, const BSONElementSet& elements,
                      const S2IndexingParams& params,
                      BSONObjSet* out) {
        for (BSONElementSet::iterator i = elements.begin(); i != elements.end(); ++i) {
            uassert(16754, "Can't parse geometry from element: " + i->toString(),
                    i->isABSONObj());
            const BSONObj &geoObj = i->Obj();

            vector<string> cells;
            bool succeeded = S2GetKeysForObject(geoObj, params, &cells);
            uassert(16755, "Can't extract geo keys from object, malformed geometry?: "
                           + document.toString(), succeeded);

            uassert(16756, "Unable to generate keys for (likely malformed) geometry: "
                    + document.toString(),
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

    /**
     * Expands array and appends items to 'out'.
     * Used by getOneLiteralKey.
     */
    void getS2LiteralKeysArray(const BSONObj& obj, BSONObjSet* out) {
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

    /**
     * If 'elt' is an array, expands elt and adds items to 'out'.
     * Otherwise, adds 'elt' as a single element.
     * Used by getLiteralKeys.
     */
    void getS2OneLiteralKey(const BSONElement& elt, BSONObjSet* out) {
        if (Array == elt.type()) {
            getS2LiteralKeysArray(elt.Obj(), out);
        } else {
            // One thing, not an array, index as-is.
            BSONObjBuilder b;
            b.appendAs(elt, "");
            out->insert(b.obj());
        }
    }

    /**
     * elements is a non-geo field.  Add the values literally, expanding arrays.
     * Used by getS2Keys.
     */
    void getS2LiteralKeys(const BSONElementSet& elements, BSONObjSet* out) {
        if (0 == elements.size()) {
            // Missing fields are indexed as null.
            BSONObjBuilder b;
            b.appendNull("");
            out->insert(b.obj());
        } else {
            for (BSONElementSet::iterator i = elements.begin(); i != elements.end(); ++i) {
                getS2OneLiteralKey(*i, out);
            }
        }
    }

    void S2KeyGenerator::getKeys(const BSONObj& obj, BSONObjSet* keys) const {
        BSONObjSet keysToAdd;

        // Does one of our documents have a geo field?
        bool haveGeoField = false;

        // We output keys in the same order as the fields we index.
        BSONObjIterator i(_keyPattern);
        while (i.more()) {
            BSONElement e = i.next();

            // First, we get the keys that this field adds.  Either they're added literally from
            // the value of the field, or they're transformed if the field is geo.
            BSONElementSet fieldElements;
            // false means Don't expand the last array, duh.
            obj.getFieldsDotted(e.fieldName(), fieldElements, false);

            BSONObjSet keysForThisField;
            if (IndexNames::GEO_2DSPHERE == e.valuestr()) {
                if (S2_INDEX_VERSION_2 == _params.indexVersion) {
                    // For V2,
                    // geo: null,
                    // geo: undefined
                    // geo: []
                    // should all behave like there is no geo field.  So we look for these cases
                    // and throw out the field elements if we find them.
                    if (1 == fieldElements.size()) {
                        BSONElement elt = *fieldElements.begin();
                        // Get the :null and :undefined cases.
                        if (elt.isNull() || Undefined == elt.type()) {
                            fieldElements.clear();
                        }
                        else if (elt.isABSONObj()) {
                            // And this is the :[] case.
                            BSONObj obj = elt.Obj();
                            if (0 == obj.nFields()) {
                                fieldElements.clear();
                            }
                        }
                    }

                    // V2 2dsphere indices require that at least one geo field to be present in a
                    // document in order to index it.
                    if (fieldElements.size() > 0) {
                        haveGeoField = true;
                    }
                }

                getS2GeoKeys(obj, fieldElements, _params, &keysForThisField);
            } else {
                getS2LiteralKeys(fieldElements, &keysForThisField);
            }

            // We expect there to be the missing field element present in the keys if data is
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

        // Make sure that if we're V2 there's at least one geo field present in the doc.
        if (S2_INDEX_VERSION_2 == _params.indexVersion) {
            if (!haveGeoField) {
                return;
            }
        }

        if (keysToAdd.size() > _params.maxKeysPerInsert) {
            warning() << "insert of geo object generated lots of keys (" << keysToAdd.size()
                      << ") consider creating larger buckets. obj="
                      << obj;
        }

        *keys = keysToAdd;
    }

}  // namespace mongo
