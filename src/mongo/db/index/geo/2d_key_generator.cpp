// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index/geo/2d_key_generator.h"

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
