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
#include "mongo/db/index/expression_params.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pdfile.h"

namespace mongo {


    TwoDKeyGenerator::TwoDKeyGenerator( const TwoDIndexingParams& params )
        : _params( params ) {
    }

    void TwoDKeyGenerator::getKeys(const BSONObj& obj, BSONObjSet* keys) const {
        getKeys( obj, keys, NULL );
    }

    void TwoDKeyGenerator::getKeys(const BSONObj& obj,
                                   BSONObjSet* keys,
                                   std::vector<BSONObj>* locs ) const {
        BSONElementMSet bSet;

        // Get all the nested location fields, but don't return individual elements from
        // the last array, if it exists.
        obj.getFieldsDotted(_params.geo.c_str(), bSet, false);

        if (bSet.empty())
            return;

        for (BSONElementMSet::iterator setI = bSet.begin(); setI != bSet.end(); ++setI) {
            BSONElement geo = *setI;

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

                    uassert(16804, mongoutils::str::stream() <<
                            "location object expected, location array not in correct format",
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
                        b.appendNull("");
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


}  // namespace mongo
