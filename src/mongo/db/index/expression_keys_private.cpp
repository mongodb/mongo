/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/db/index/expression_keys_private.h"

#include <utility>

#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/fts/fts_index_format.h"
#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/geo/s2.h"
#include "mongo/db/index/2d_common.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/index_names.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "third_party/s2/s2cell.h"
#include "third_party/s2/s2regioncoverer.h"

namespace {

using namespace mongo;

namespace dps = ::mongo::dotted_path_support;

//
// Helper functions for getHaystackKeys
//

/**
 * Build a new BSONObj with root in it.  If e is non-empty, append that to the key.
 * Insert the BSONObj into keys.
 * Used by getHaystackKeys.
 */
void addKey(const string& root, const BSONElement& e, BSONObjSet* keys) {
    BSONObjBuilder buf;
    buf.append("", root);

    if (e.eoo())
        buf.appendNull("");
    else
        buf.appendAs(e, "");

    keys->insert(buf.obj());
}

//
// Helper functions for getS2Keys
//

Status S2GetKeysForElement(const BSONElement& element,
                           const S2IndexingParams& params,
                           vector<S2CellId>* out) {
    GeometryContainer geoContainer;
    Status status = geoContainer.parseFromStorage(element);
    if (!status.isOK())
        return status;

    S2RegionCoverer coverer;
    params.configureCoverer(geoContainer, &coverer);

    // Don't index big polygon
    if (geoContainer.getNativeCRS() == STRICT_SPHERE) {
        return Status(ErrorCodes::BadValue, "can't index geometry with strict winding order");
    }

    // Only certain geometries can be indexed in the old index format S2_INDEX_VERSION_1.  See
    // definition of S2IndexVersion for details.
    if (params.indexVersion == S2_INDEX_VERSION_1 && !geoContainer.isSimpleContainer()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "given geometry can't be indexed in the old index format");
    }

    // Project the geometry into spherical space
    if (!geoContainer.supportsProject(SPHERE)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "can't project geometry into spherical CRS: "
                                    << element.toString(false));
    }
    geoContainer.projectInto(SPHERE);

    invariant(geoContainer.hasS2Region());

    coverer.GetCovering(geoContainer.getS2Region(), out);
    return Status::OK();
}


/**
 * Fills 'out' with the S2 keys that should be generated for 'elements' in a 2dsphere index.
 *
 * Returns true if an indexed element of the document uses multiple cells for its covering, and
 * returns false otherwise.
 */
bool getS2GeoKeys(const BSONObj& document,
                  const BSONElementSet& elements,
                  const S2IndexingParams& params,
                  BSONObjSet* out) {
    bool everGeneratedMultipleCells = false;
    for (BSONElementSet::iterator i = elements.begin(); i != elements.end(); ++i) {
        vector<S2CellId> cells;
        Status status = S2GetKeysForElement(*i, params, &cells);
        uassert(16755,
                str::stream() << "Can't extract geo keys: " << document << "  " << status.reason(),
                status.isOK());

        uassert(16756,
                "Unable to generate keys for (likely malformed) geometry: " + document.toString(),
                cells.size() > 0);

        for (vector<S2CellId>::const_iterator it = cells.begin(); it != cells.end(); ++it) {
            out->insert(S2CellIdToIndexKey(*it, params.indexVersion));
        }

        everGeneratedMultipleCells = everGeneratedMultipleCells || cells.size() > 1;
    }

    if (0 == out->size()) {
        BSONObjBuilder b;
        b.appendNull("");
        out->insert(b.obj());
    }
    return everGeneratedMultipleCells;
}

/**
 * Fills 'out' with the keys that should be generated for an array value 'obj' in a 2dsphere index.
 * A key is generated for each element of the array value 'obj'.
 *
 * Returns true if 'obj' contains more than one element, and returns false otherwise.
 */
bool getS2LiteralKeysArray(const BSONObj& obj, const CollatorInterface* collator, BSONObjSet* out) {
    BSONObjIterator objIt(obj);
    if (!objIt.more()) {
        // Empty arrays are indexed as undefined.
        BSONObjBuilder b;
        b.appendUndefined("");
        out->insert(b.obj());
    } else {
        // Non-empty arrays are exploded.
        size_t nArrElems = 0;
        while (objIt.more()) {
            BSONObjBuilder b;
            CollationIndexKey::collationAwareIndexKeyAppend(objIt.next(), collator, &b);
            out->insert(b.obj());
            ++nArrElems;
        }

        if (nArrElems > 1) {
            return true;
        }
    }
    return false;
}

/**
 * Fills 'out' with the keys that should be generated for a value 'elt' in a 2dsphere index. If
 * 'elt' is an array value, then a key is generated for each element of the array value 'obj'.
 *
 * Returns true if 'elt' is an array value that contains more than one element, and returns false
 * otherwise.
 */
bool getS2OneLiteralKey(const BSONElement& elt,
                        const CollatorInterface* collator,
                        BSONObjSet* out) {
    if (Array == elt.type()) {
        return getS2LiteralKeysArray(elt.Obj(), collator, out);
    } else {
        // One thing, not an array, index as-is.
        BSONObjBuilder b;
        CollationIndexKey::collationAwareIndexKeyAppend(elt, collator, &b);
        out->insert(b.obj());
    }
    return false;
}

/**
 * Fills 'out' with the non-geo keys that should be generated for 'elements' in a 2dsphere index. If
 * any element in 'elements' is an array value, then a key is generated for each element of that
 * array value.
 *
 * Returns true if any element of 'elements' is an array value that contains more than one element,
 * and returns false otherwise.
 */
bool getS2LiteralKeys(const BSONElementSet& elements,
                      const CollatorInterface* collator,
                      BSONObjSet* out) {
    bool indexedArrayValueWithMultipleElements = false;
    if (0 == elements.size()) {
        // Missing fields are indexed as null.
        BSONObjBuilder b;
        b.appendNull("");
        out->insert(b.obj());
    } else {
        for (BSONElementSet::iterator i = elements.begin(); i != elements.end(); ++i) {
            const bool thisElemIsArrayWithMultipleElements = getS2OneLiteralKey(*i, collator, out);
            indexedArrayValueWithMultipleElements =
                indexedArrayValueWithMultipleElements || thisElemIsArrayWithMultipleElements;
        }
    }
    return indexedArrayValueWithMultipleElements;
}

}  // namespace

namespace mongo {

using std::pair;
using std::string;
using std::vector;

// static
void ExpressionKeysPrivate::get2DKeys(const BSONObj& obj,
                                      const TwoDIndexingParams& params,
                                      BSONObjSet* keys,
                                      std::vector<BSONObj>* locs) {
    BSONElementMSet bSet;

    // Get all the nested location fields, but don't return individual elements from
    // the last array, if it exists.
    dps::extractAllElementsAlongPath(obj, params.geo.c_str(), bSet, false);

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

                uassert(16804,
                        mongoutils::str::stream()
                            << "location object expected, location array not in correct format",
                        locElement.isABSONObj());

                locObj = locElement.embeddedObject();
                if (locObj.isEmpty())
                    continue;
            }

            BSONObjBuilder b(64);

            // Remember the actual location object if needed
            if (locs)
                locs->push_back(locObj);

            // Stop if we don't need to get anything but location objects
            if (!keys) {
                if (singleElement)
                    break;
                else
                    continue;
            }

            params.geoHashConverter->hash(locObj, &obj).appendHashMin(&b, "");

            // Go through all the other index keys
            for (vector<pair<string, int>>::const_iterator i = params.other.begin();
                 i != params.other.end();
                 ++i) {
                // Get *all* fields for the index key
                BSONElementSet eSet;
                dps::extractAllElementsAlongPath(obj, i->first, eSet);

                if (eSet.size() == 0)
                    b.appendNull("");
                else if (eSet.size() == 1)
                    b.appendAs(*(eSet.begin()), "");
                else {
                    // If we have more than one key, store as an array of the objects
                    BSONArrayBuilder aBuilder;

                    for (BSONElementSet::iterator ei = eSet.begin(); ei != eSet.end(); ++ei) {
                        aBuilder.append(*ei);
                    }

                    b.append("", aBuilder.arr());
                }
            }
            keys->insert(b.obj());
            if (singleElement)
                break;
        }
    }
}

// static
void ExpressionKeysPrivate::getFTSKeys(const BSONObj& obj,
                                       const fts::FTSSpec& ftsSpec,
                                       BSONObjSet* keys) {
    fts::FTSIndexFormat::getKeys(ftsSpec, obj, keys);
}

// static
void ExpressionKeysPrivate::getHashKeys(const BSONObj& obj,
                                        const string& hashedField,
                                        HashSeed seed,
                                        int hashVersion,
                                        bool isSparse,
                                        const CollatorInterface* collator,
                                        BSONObjSet* keys) {
    const char* cstr = hashedField.c_str();
    BSONElement fieldVal = dps::extractElementAtPath(obj, cstr);

    // Convert strings to comparison keys.
    BSONObj fieldValObj;
    if (!fieldVal.eoo()) {
        BSONObjBuilder bob;
        CollationIndexKey::collationAwareIndexKeyAppend(fieldVal, collator, &bob);
        fieldValObj = bob.obj();
        fieldVal = fieldValObj.firstElement();
    }

    uassert(16766,
            "Error: hashed indexes do not currently support array values",
            fieldVal.type() != Array);

    if (!fieldVal.eoo()) {
        BSONObj key = BSON("" << makeSingleHashKey(fieldVal, seed, hashVersion));
        keys->insert(key);
    } else if (!isSparse) {
        BSONObj nullObj = BSON("" << BSONNULL);
        keys->insert(BSON("" << makeSingleHashKey(nullObj.firstElement(), seed, hashVersion)));
    }
}

// static
long long int ExpressionKeysPrivate::makeSingleHashKey(const BSONElement& e, HashSeed seed, int v) {
    massert(16767, "Only HashVersion 0 has been defined", v == 0);
    return BSONElementHasher::hash64(e, seed);
}

// static
void ExpressionKeysPrivate::getHaystackKeys(const BSONObj& obj,
                                            const std::string& geoField,
                                            const std::vector<std::string>& otherFields,
                                            double bucketSize,
                                            BSONObjSet* keys) {
    BSONElement loc = dps::extractElementAtPath(obj, geoField);

    if (loc.eoo()) {
        return;
    }

    // NOTE: We explicitly test nFields >= 2 to support legacy users who may have indexed
    // (intentionally or unintentionally) objects/arrays with more than two fields.
    uassert(16775,
            str::stream() << "cannot extract [lng, lat] array or object from " << obj,
            loc.isABSONObj() && loc.Obj().nFields() >= 2);

    string root;
    {
        BSONObjIterator i(loc.Obj());
        BSONElement x = i.next();
        BSONElement y = i.next();
        root = makeHaystackString(hashHaystackElement(x, bucketSize),
                                  hashHaystackElement(y, bucketSize));
    }

    verify(otherFields.size() == 1);

    BSONElementSet all;

    // The object we're indexing may be an array.
    dps::extractAllElementsAlongPath(obj, otherFields[0], all);

    if (all.size() == 0) {
        // We're indexing a document that doesn't have the secondary non-geo field present.
        // XXX: do we want to add this even if all.size() > 0?  result:empty search terms
        // match everything instead of only things w/empty search terms)
        addKey(root, BSONElement(), keys);
    } else {
        // Ex:If our secondary field is type: "foo" or type: {a:"foo", b:"bar"},
        // all.size()==1.  We can query on the complete field.
        // Ex: If our secondary field is type: ["A", "B"] all.size()==2 and all has values
        // "A" and "B".  The query looks for any of the fields in the array.
        for (BSONElementSet::iterator i = all.begin(); i != all.end(); ++i) {
            addKey(root, *i, keys);
        }
    }
}

// static
int ExpressionKeysPrivate::hashHaystackElement(const BSONElement& e, double bucketSize) {
    uassert(16776, "geo field is not a number", e.isNumber());
    double d = e.numberDouble();
    d += 180;
    d /= bucketSize;
    return static_cast<int>(d);
}

// static
std::string ExpressionKeysPrivate::makeHaystackString(int hashedX, int hashedY) {
    mongoutils::str::stream ss;
    ss << hashedX << "_" << hashedY;
    return ss;
}

void ExpressionKeysPrivate::getS2Keys(const BSONObj& obj,
                                      const BSONObj& keyPattern,
                                      const S2IndexingParams& params,
                                      BSONObjSet* keys,
                                      MultikeyPaths* multikeyPaths) {
    BSONObjSet keysToAdd;

    // Does one of our documents have a geo field?
    bool haveGeoField = false;

    if (multikeyPaths) {
        invariant(multikeyPaths->empty());
        multikeyPaths->resize(keyPattern.nFields());
    }

    size_t posInIdx = 0;

    // We output keys in the same order as the fields we index.
    for (const auto keyElem : keyPattern) {
        // First, we get the keys that this field adds.  Either they're added literally from
        // the value of the field, or they're transformed if the field is geo.
        BSONElementSet fieldElements;
        const bool expandArrayOnTrailingField = false;
        std::set<size_t>* arrayComponents = multikeyPaths ? &(*multikeyPaths)[posInIdx] : nullptr;
        dps::extractAllElementsAlongPath(
            obj, keyElem.fieldName(), fieldElements, expandArrayOnTrailingField, arrayComponents);

        // Trailing array values aren't being expanded, so we still need to determine whether the
        // last component of the indexed path 'keyElem.fieldName()' causes the index to be multikey.
        // We say that it does if
        //   (a) the last component of the indexed path ever refers to an array value containing
        //       multiple elements, or if
        //   (b) the last component of the indexed path ever refers to GeoJSON data that requires
        //       multiple cells for its covering.
        bool lastPathComponentCausesIndexToBeMultikey;
        BSONObjSet keysForThisField;
        if (IndexNames::GEO_2DSPHERE == keyElem.valuestr()) {
            if (params.indexVersion >= S2_INDEX_VERSION_2) {
                // For >= V2,
                // geo: null,
                // geo: undefined
                // geo: []
                // should all behave like there is no geo field.  So we look for these cases and
                // throw out the field elements if we find them.
                if (1 == fieldElements.size()) {
                    BSONElement elt = *fieldElements.begin();
                    // Get the :null and :undefined cases.
                    if (elt.isNull() || Undefined == elt.type()) {
                        fieldElements.clear();
                    } else if (elt.isABSONObj()) {
                        // And this is the :[] case.
                        BSONObj obj = elt.Obj();
                        if (0 == obj.nFields()) {
                            fieldElements.clear();
                        }
                    }
                }

                // >= V2 2dsphere indices require that at least one geo field to be present in a
                // document in order to index it.
                if (fieldElements.size() > 0) {
                    haveGeoField = true;
                }
            }

            lastPathComponentCausesIndexToBeMultikey =
                getS2GeoKeys(obj, fieldElements, params, &keysForThisField);
        } else {
            lastPathComponentCausesIndexToBeMultikey =
                getS2LiteralKeys(fieldElements, params.collator, &keysForThisField);
        }

        // We expect there to be the missing field element present in the keys if data is
        // missing.  So, this should be non-empty.
        verify(!keysForThisField.empty());

        if (multikeyPaths && lastPathComponentCausesIndexToBeMultikey) {
            const size_t pathLengthOfThisField = FieldRef{keyElem.fieldNameStringData()}.numParts();
            invariant(pathLengthOfThisField > 0);
            (*multikeyPaths)[posInIdx].insert(pathLengthOfThisField - 1);
        }

        // We take the Cartesian product of all of the keys.  This requires that we have
        // some keys to take the Cartesian product with.  If keysToAdd.empty(), we
        // initialize it.
        if (keysToAdd.empty()) {
            keysToAdd = keysForThisField;
            ++posInIdx;
            continue;
        }

        BSONObjSet updatedKeysToAdd;
        for (BSONObjSet::const_iterator it = keysToAdd.begin(); it != keysToAdd.end(); ++it) {
            for (BSONObjSet::const_iterator newIt = keysForThisField.begin();
                 newIt != keysForThisField.end();
                 ++newIt) {
                BSONObjBuilder b;
                b.appendElements(*it);
                b.append(newIt->firstElement());
                updatedKeysToAdd.insert(b.obj());
            }
        }
        keysToAdd = updatedKeysToAdd;
        ++posInIdx;
    }

    // Make sure that if we're >= V2 there's at least one geo field present in the doc.
    if (params.indexVersion >= S2_INDEX_VERSION_2) {
        if (!haveGeoField) {
            return;
        }
    }

    if (keysToAdd.size() > params.maxKeysPerInsert) {
        warning() << "Insert of geo object generated a high number of keys."
                  << " num keys: " << keysToAdd.size() << " obj inserted: " << obj;
    }

    *keys = keysToAdd;
}

}  // namespace mongo
