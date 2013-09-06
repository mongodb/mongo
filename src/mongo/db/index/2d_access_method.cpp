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

#include "mongo/db/index/2d_access_method.h"

#include <string>
#include <vector>

#include "mongo/db/geo/core.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index/2d_common.h"
#include "mongo/db/index/2d_index_cursor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    static double configValueWithDefault(IndexDescriptor *desc, const string& name, double def) {
        BSONElement e = desc->getInfoElement(name);
        if (e.isNumber()) { return e.numberDouble(); }
        return def;
    }

    TwoDAccessMethod::TwoDAccessMethod(IndexDescriptor* descriptor)
        : BtreeBasedAccessMethod(descriptor) {

        BSONObjIterator i(descriptor->keyPattern());
        while (i.more()) {
            BSONElement e = i.next();
            if (e.type() == String && IndexNames::GEO_2D == e.valuestr()) {
                uassert(16800, "can't have 2 geo fields", _params.geo.size() == 0);
                uassert(16801, "2d has to be first in index", _params.other.size() == 0);
                _params.geo = e.fieldName();
            } else {
                int order = 1;
                if (e.isNumber()) {
                    order = static_cast<int>(e.Number());
                }
                _params.other.push_back(make_pair(e.fieldName(), order));
            }
        }
        uassert(16802, "no geo field specified", _params.geo.size());

        double bits =  configValueWithDefault(_descriptor, "bits", 26);  // for lat/long, ~ 1ft
        uassert(16803, "bits in geo index must be between 1 and 32", bits > 0 && bits <= 32);

        GeoHashConverter::Parameters params;
        params.bits = static_cast<unsigned>(bits);
        params.max = configValueWithDefault(_descriptor, "max", 180.0);
        params.min = configValueWithDefault(_descriptor, "min", -180.0);
        double numBuckets = (1024 * 1024 * 1024 * 4.0);
        params.scaling = numBuckets / (params.max - params.min);

        _params.geoHashConverter.reset(new GeoHashConverter(params));

        BSONObjBuilder b;
        b.appendNull("");
        _nullObj = b.obj();
        _nullElt = _nullObj.firstElement();
    }

    /** Finds the key objects to put in an index */
    void TwoDAccessMethod::getKeys(const BSONObj& obj, BSONObjSet* keys) {
        getKeys(obj, keys, NULL);
    }

    /** Finds all locations in a geo-indexed object */
    void TwoDAccessMethod::getKeys(const BSONObj& obj, vector<BSONObj>& locs) const {
        getKeys(obj, NULL, &locs);
    }

    /** Finds the key objects and/or locations for a geo-indexed object */
    void TwoDAccessMethod::getKeys(const BSONObj &obj, BSONObjSet* keys,
                                   vector<BSONObj>* locs) const {
        BSONElementMSet bSet;

        // Get all the nested location fields, but don't return individual elements from
        // the last array, if it exists.
        obj.getFieldsDotted(_params.geo.c_str(), bSet, false);

        if (bSet.empty())
            return;

        for (BSONElementMSet::iterator setI = bSet.begin(); setI != bSet.end(); ++setI) {
            BSONElement geo = *setI;

            GEODEBUG("Element " << geo << " found for query " << _geo.c_str());

            if (geo.eoo() || !geo.isABSONObj())
                continue;

            //
            // Grammar for location lookup:
            // locs ::= [loc,loc,...,loc]|{<k>:loc,<k>:loc,...,<k>:loc}|loc
            // loc  ::= { <k1> : #, <k2> : # }|[#, #]|{}
            //
            // Empty locations are ignored, preserving single-location semantics
            //

            BSONObj embed = geo.embeddedObject();
            if (embed.isEmpty())
                continue;

            // Differentiate between location arrays and locations
            // by seeing if the first element value is a number
            bool singleElement = embed.firstElement().isNumber();

            BSONObjIterator oi(embed);

            while (oi.more()) {
                BSONObj locObj;

                if (singleElement) {
                    locObj = embed;
                } else {
                    BSONElement locElement = oi.next();

                    uassert(16804, str::stream() << "location object expected, location "
                                                "array not in correct format",
                            locElement.isABSONObj());

                    locObj = locElement.embeddedObject();
                    if(locObj.isEmpty())
                        continue;
                }

                BSONObjBuilder b(64);

                // Remember the actual location object if needed
                if (locs)
                    locs->push_back(locObj);

                // Stop if we don't need to get anything but location objects
                if (!keys) {
                    if (singleElement) break;
                    else continue;
                }

                _params.geoHashConverter->hash(locObj, &obj).appendToBuilder(&b, "");
   
                // Go through all the other index keys
                for (vector<pair<string, int> >::const_iterator i = _params.other.begin();
                     i != _params.other.end(); ++i) {
                    // Get *all* fields for the index key
                    BSONElementSet eSet;
                    obj.getFieldsDotted(i->first, eSet);

                    if (eSet.size() == 0)
                        b.appendAs(_nullElt, "");
                    else if (eSet.size() == 1)
                        b.appendAs(*(eSet.begin()), "");
                    else {
                        // If we have more than one key, store as an array of the objects
                        BSONArrayBuilder aBuilder;

                        for (BSONElementSet::iterator ei = eSet.begin(); ei != eSet.end();
                             ++ei) {
                            aBuilder.append(*ei);
                        }

                        b.append("", aBuilder.arr());
                    }
                }
                keys->insert(b.obj());
                if(singleElement) break;
            }
        }
    }

    Status TwoDAccessMethod::newCursor(IndexCursor** out) {
        *out = new TwoDIndexCursor(this);
        return Status::OK();
    }

}  // namespace mongo
