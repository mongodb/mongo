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
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/geo/s2common.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index/s2_index_cursor.h"
#include "mongo/db/jsobj.h"

namespace mongo {
    static int configValueWithDefault(IndexDescriptor *desc, const string& name, int def) {
        BSONElement e = desc->getInfoElement(name);
        if (e.isNumber()) { return e.numberInt(); }
        return def;
    }

    S2AccessMethod::S2AccessMethod(IndexDescriptor *descriptor)
        : BtreeBasedAccessMethod(descriptor) {

        // Set up basic params.
        _params.maxKeysPerInsert = 200;
        // This is advisory.
        _params.maxCellsInCovering = 50;
        // Near distances are specified in meters...sometimes.
        _params.radius = kRadiusOfEarthInMeters;
        // These are not advisory.
        _params.finestIndexedLevel = configValueWithDefault(descriptor, "finestIndexedLevel",
            S2::kAvgEdge.GetClosestLevel(500.0 / _params.radius));
        _params.coarsestIndexedLevel = configValueWithDefault(descriptor, "coarsestIndexedLevel",
            S2::kAvgEdge.GetClosestLevel(100 * 1000.0 / _params.radius));
        uassert(16747, "coarsestIndexedLevel must be >= 0", _params.coarsestIndexedLevel >= 0);
        uassert(16748, "finestIndexedLevel must be <= 30", _params.finestIndexedLevel <= 30);
        uassert(16749, "finestIndexedLevel must be >= coarsestIndexedLevel",
                _params.finestIndexedLevel >= _params.coarsestIndexedLevel);

        int geoFields = 0;

        // Categorize the fields we're indexing and make sure we have a geo field.
        BSONObjIterator i(descriptor->keyPattern());
        while (i.more()) {
            BSONElement e = i.next();
            if (e.type() == String && IndexNames::GEO_2DSPHERE == e.String() ) {
                ++geoFields;
            }
            else {
                // We check for numeric in 2d, so that's the check here
                uassert( 16823, (string)"Cannot use " + IndexNames::GEO_2DSPHERE +
                                    " index with other special index types: " + e.toString(),
                         e.isNumber() );
            }
        }
        uassert(16750, "Expect at least one geo field, spec=" + descriptor->keyPattern().toString(),
                geoFields >= 1);
    }

    Status S2AccessMethod::newCursor(IndexCursor** out) {
        *out = new S2IndexCursor(_params, _descriptor);
        return Status::OK();
    }

    void S2AccessMethod::getKeys(const BSONObj& obj, BSONObjSet* keys) {
        BSONObjSet keysToAdd;
        // We output keys in the same order as the fields we index.
        BSONObjIterator i(_descriptor->keyPattern());
        while (i.more()) {
            BSONElement e = i.next();

            // First, we get the keys that this field adds.  Either they're added literally from
            // the value of the field, or they're transformed if the field is geo.
            BSONElementSet fieldElements;
            // false means Don't expand the last array, duh.
            obj.getFieldsDotted(e.fieldName(), fieldElements, false);

            BSONObjSet keysForThisField;
            if (IndexNames::GEO_2DSPHERE == e.valuestr()) {
                // We can't ever return documents that don't have geometry so don't bother indexing
                // them.
                if (fieldElements.empty()) { return; }
                getGeoKeys(obj, fieldElements, &keysForThisField);
            } else {
                getLiteralKeys(fieldElements, &keysForThisField);
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

        if (keysToAdd.size() > _params.maxKeysPerInsert) {
            warning() << "insert of geo object generated lots of keys (" << keysToAdd.size()
                << ") consider creating larger buckets. obj="
                << obj;
        }

        *keys = keysToAdd;
    }

    // Get the index keys for elements that are GeoJSON.
    void S2AccessMethod::getGeoKeys(const BSONObj& document, const BSONElementSet& elements,
                                    BSONObjSet* out) const {
        for (BSONElementSet::iterator i = elements.begin(); i != elements.end(); ++i) {
            uassert(16754, "Can't parse geometry from element: " + i->toString(),
                    i->isABSONObj());
            const BSONObj &geoObj = i->Obj();

            vector<string> cells;
            bool succeeded = S2SearchUtil::getKeysForObject(geoObj, _params, &cells);
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

    void S2AccessMethod::getLiteralKeysArray(const BSONObj& obj, BSONObjSet* out) const {
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

    void S2AccessMethod::getOneLiteralKey(const BSONElement& elt, BSONObjSet* out) const {
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
    void S2AccessMethod::getLiteralKeys(const BSONElementSet& elements,
                                        BSONObjSet* out) const {
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

}  // namespace mongo
