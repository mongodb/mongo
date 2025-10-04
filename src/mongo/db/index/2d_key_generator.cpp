/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/index/2d_key_generator.h"

#include "mongo/db/query/bson/multikey_dotted_path_support.h"

namespace mongo::index2d {
namespace mdps = ::mongo::multikey_dotted_path_support;

void get2DKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
               const BSONObj& obj,
               const TwoDIndexingParams& params,
               KeyStringSet* keys,
               key_string::Version keyStringVersion,
               Ordering ordering,
               const boost::optional<RecordId>& id) {
    BSONElementMultiSet bSet;

    // Get all the nested location fields, but don't return individual elements from
    // the last array, if it exists.
    mdps::extractAllElementsAlongPath(obj, params.geo.c_str(), bSet, false);

    if (bSet.empty())
        return;

    auto keysSequence = keys->extract_sequence();
    for (BSONElementMultiSet::iterator setI = bSet.begin(); setI != bSet.end(); ++setI) {
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

                uassert(16804,
                        str::stream()
                            << "location object expected, location array not in correct format",
                        locElement.isABSONObj());

                locObj = locElement.embeddedObject();
                if (locObj.isEmpty())
                    continue;
            }

            key_string::PooledBuilder keyString(pooledBufferBuilder, keyStringVersion, ordering);
            params.geoHashConverter->hash(locObj, &obj).appendHashMin(&keyString);

            // Go through all the other index keys
            for (std::vector<std::pair<std::string, int>>::const_iterator i = params.other.begin();
                 i != params.other.end();
                 ++i) {
                // Get *all* fields for the index key
                BSONElementSet eSet;
                mdps::extractAllElementsAlongPath(obj, i->first, eSet);

                if (eSet.size() == 0)
                    keyString.appendNull();
                else if (eSet.size() == 1)
                    keyString.appendBSONElement(*(eSet.begin()));
                else {
                    // If we have more than one key, store as an array of the objects
                    keyString.appendSetAsArray(eSet);
                }
            }

            if (id) {
                keyString.appendRecordId(*id);
            }
            keysSequence.push_back(keyString.release());
            if (singleElement)
                break;
        }
    }
    keys->adopt_sequence(std::move(keysSequence));
}
}  // namespace mongo::index2d
