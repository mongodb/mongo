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

#include "mongo/db/index/s2_key_generator.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/query/bson/multikey_dotted_path_support.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_dotted_path_support.h"

#include <s2cellid.h>
#include <s2regioncoverer.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

namespace mongo::index2dsphere {
namespace mdps = ::mongo::multikey_dotted_path_support;

MONGO_FAIL_POINT_DEFINE(relaxIndexMaxNumGeneratedKeysPerDocument);

namespace {

// Internal exception to abort key generation. Should be translated to something user friendly and
// not escape past this file.
class MaxKeysExceededException final : public DBException {
public:
    MaxKeysExceededException()
        : DBException(Status(ErrorCodes::CannotBuildIndexKeys,
                             "Maximum number of generated keys exceeded.")) {}

private:
    void defineOnlyInFinalSubclassToPreventSlicing() final {}
};

Status S2GetKeysForElement(const BSONElement& element,
                           const S2IndexingParams& params,
                           std::vector<S2CellId>* out) {
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

    tassert(9911905, "", geoContainer.hasS2Region());

    coverer.GetCovering(geoContainer.getS2Region(), out);
    return Status::OK();
}

/*
 * We take the cartesian product of all keys when appending.
 */
void appendToS2Keys(const std::vector<key_string::HeapBuilder>& existingKeys,
                    std::vector<key_string::HeapBuilder>* out,
                    key_string::Version keyStringVersion,
                    SortedDataIndexAccessMethod::GetKeysContext context,
                    Ordering ordering,
                    size_t maxKeys,
                    const std::function<void(key_string::HeapBuilder&)>& fn) {
    if (context == SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys &&
        existingKeys.size() + out->size() > maxKeys) {
        if (!relaxIndexMaxNumGeneratedKeysPerDocument.shouldFail()) {
            throw MaxKeysExceededException();
        }
    }
    if (existingKeys.empty()) {
        /*
         * This is the base case when the keys for the first field are generated.
         */
        out->emplace_back(keyStringVersion, ordering);
        fn(out->back());
    }
    for (const auto& ks : existingKeys) {
        /*
         * We copy all of the existing keys and perform 'fn' on each copy.
         */
        out->emplace_back(ks);
        fn(out->back());
    }
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
                  const std::vector<key_string::HeapBuilder>& keysToAdd,
                  std::vector<key_string::HeapBuilder>* out,
                  key_string::Version keyStringVersion,
                  SortedDataIndexAccessMethod::GetKeysContext context,
                  Ordering ordering,
                  size_t maxKeys) {
    bool everGeneratedMultipleCells = false;
    for (BSONElementSet::iterator i = elements.begin(); i != elements.end(); ++i) {
        std::vector<S2CellId> cells;
        Status status = S2GetKeysForElement(*i, params, &cells);
        uassert(16755,
                str::stream() << "Can't extract geo keys: " << document << "  " << status.reason(),
                status.isOK());

        uassert(16756,
                "Unable to generate keys for (likely malformed) geometry: " + document.toString(),
                cells.size() > 0);

        // We'll be taking the cartesian product of cells and keysToAdd, make sure the output won't
        // be too big.
        if (context == SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys &&
            cells.size() * keysToAdd.size() > maxKeys) {
            if (!relaxIndexMaxNumGeneratedKeysPerDocument.shouldFail()) {
                throw MaxKeysExceededException();
            }
        }

        for (std::vector<S2CellId>::const_iterator it = cells.begin(); it != cells.end(); ++it) {
            index2dsphere::S2CellIdToIndexKeyStringAppend(
                *it, params.indexVersion, keysToAdd, out, keyStringVersion, ordering);
        }

        everGeneratedMultipleCells = everGeneratedMultipleCells || cells.size() > 1;
    }

    if (0 == out->size()) {
        appendToS2Keys(keysToAdd,
                       out,
                       keyStringVersion,
                       context,
                       ordering,
                       maxKeys,
                       [](key_string::HeapBuilder& ks) { ks.appendNull(); });
    }
    return everGeneratedMultipleCells;
}

/**
 * Fills 'out' with the S2 keys that should be generated for 'elements' in a 2dsphere_bucket index.
 *
 * Returns true if an indexed element of the document uses multiple cells for its covering, and
 * returns false otherwise.
 */
bool getS2BucketGeoKeys(const BSONObj& document,
                        const BSONElementSet& elements,
                        const S2IndexingParams& params,
                        const std::vector<key_string::HeapBuilder>& keysToAdd,
                        std::vector<key_string::HeapBuilder>* out,
                        key_string::Version keyStringVersion,
                        SortedDataIndexAccessMethod::GetKeysContext context,
                        Ordering ordering,
                        size_t maxKeys) {
    bool generatedMultipleCells = false;
    if (!elements.empty()) {
        /**
         * We're going to build a MultiPoint GeoJSON that contains all the distinct points in the
         * bucket. The S2RegionCoverer will index that the best it can within the cell limits we
         * impose. In order to re-use S2GetKeysForElement, we need to wrap the GeoJSON as a
         * sub-document of our constructed BSON so we can pass it as an element. In the end, we're
         * building a document like:
         * {
         *   "shape": {
         *     "type": "MultiPoint",
         *     "coords": [
         *        ...
         *     ]
         *   }
         * }
         */
        BSONObjBuilder builder;
        {
            BSONObjBuilder shape(builder.subobjStart("shape"));
            shape.append("type", "MultiPoint");
            BSONArrayBuilder coordinates(shape.subarrayStart("coordinates"));
            for (BSONElementSet::iterator i = elements.begin(); i != elements.end(); ++i) {
                GeometryContainer container;
                auto status = container.parseFromStorage(*i, false);
                uassert(183934,
                        str::stream() << "Can't extract geo keys: " << status.reason(),
                        status.isOK());
                uassert(183493,
                        str::stream()
                            << "Time-series collections '2dsphere' indexes only support point data",
                        container.isPoint());

                auto point = container.getPoint();
                BSONArrayBuilder pointData(coordinates.subarrayStart());
                coordinates.append(point.oldPoint.x);
                coordinates.append(point.oldPoint.y);
            }
        }
        BSONObj geometry = builder.obj();

        std::vector<S2CellId> cells;
        Status status = S2GetKeysForElement(geometry.firstElement(), params, &cells);
        uassert(
            167551, str::stream() << "Can't extract geo keys: " << status.reason(), status.isOK());

        uassert(167561,
                str::stream() << "Unable to generate keys for (likely malformed) geometry",
                cells.size() > 0);

        // We'll be taking the cartesian product of cells and keysToAdd, make sure the output won't
        // be too big.
        if (context == SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys &&
            cells.size() * keysToAdd.size() > maxKeys) {
            if (!relaxIndexMaxNumGeneratedKeysPerDocument.shouldFail()) {
                throw MaxKeysExceededException();
            }
        }

        for (std::vector<S2CellId>::const_iterator it = cells.begin(); it != cells.end(); ++it) {
            index2dsphere::S2CellIdToIndexKeyStringAppend(
                *it, params.indexVersion, keysToAdd, out, keyStringVersion, ordering);
        }

        generatedMultipleCells = cells.size() > 1;
    }

    if (0 == out->size()) {
        appendToS2Keys(keysToAdd,
                       out,
                       keyStringVersion,
                       context,
                       ordering,
                       maxKeys,
                       [](key_string::HeapBuilder& ks) { ks.appendNull(); });
    }
    return generatedMultipleCells;
}

/**
 * Fills 'out' with the keys that should be generated for an array value 'obj' in a 2dsphere index.
 * A key is generated for each element of the array value 'obj'.
 */
void getS2LiteralKeysArray(const BSONObj& obj,
                           const CollatorInterface* collator,
                           const std::vector<key_string::HeapBuilder>& keysToAdd,
                           std::vector<key_string::HeapBuilder>* out,
                           key_string::Version keyStringVersion,
                           SortedDataIndexAccessMethod::GetKeysContext context,
                           Ordering ordering,
                           size_t maxKeys) {
    BSONObjIterator objIt(obj);
    if (!objIt.more()) {
        // Empty arrays are indexed as undefined.
        appendToS2Keys(keysToAdd,
                       out,
                       keyStringVersion,
                       context,
                       ordering,
                       maxKeys,
                       [](key_string::HeapBuilder& ks) { ks.appendUndefined(); });
    } else {
        // Non-empty arrays are exploded.
        while (objIt.more()) {
            const auto elem = objIt.next();
            appendToS2Keys(keysToAdd,
                           out,
                           keyStringVersion,
                           context,
                           ordering,
                           maxKeys,
                           [&](key_string::HeapBuilder& ks) {
                               if (collator) {
                                   ks.appendBSONElement(elem, [&](StringData stringData) {
                                       return collator->getComparisonString(stringData);
                                   });
                               } else {
                                   ks.appendBSONElement(elem);
                               }
                           });
        }
    }
}

/**
 * Fills 'out' with the keys that should be generated for a value 'elt' in a 2dsphere index. If
 * 'elt' is an array value, then a key is generated for each element of the array value 'obj'.
 *
 * Returns true if 'elt' is an array value and returns false otherwise.
 */
bool getS2OneLiteralKey(const BSONElement& elt,
                        const CollatorInterface* collator,
                        const std::vector<key_string::HeapBuilder>& keysToAdd,
                        std::vector<key_string::HeapBuilder>* out,
                        key_string::Version keyStringVersion,
                        SortedDataIndexAccessMethod::GetKeysContext context,
                        Ordering ordering,
                        size_t maxKeys) {
    if (BSONType::array == elt.type()) {
        getS2LiteralKeysArray(
            elt.Obj(), collator, keysToAdd, out, keyStringVersion, context, ordering, maxKeys);
        return true;
    } else {
        // One thing, not an array, index as-is.
        appendToS2Keys(keysToAdd,
                       out,
                       keyStringVersion,
                       context,
                       ordering,
                       maxKeys,
                       [&](key_string::HeapBuilder& ks) {
                           if (collator) {
                               ks.appendBSONElement(elt, [&](StringData stringData) {
                                   return collator->getComparisonString(stringData);
                               });
                           } else {
                               ks.appendBSONElement(elt);
                           }
                       });
    }
    return false;
}

/**
 * Fills 'out' with the non-geo keys that should be generated for 'elements' in a 2dsphere
 * index. If any element in 'elements' is an array value, then a key is generated for each
 * element of that array value.
 *
 * Returns true if any element of 'elements' is an array value and returns false otherwise.
 */
bool getS2LiteralKeys(const BSONElementSet& elements,
                      const CollatorInterface* collator,
                      const std::vector<key_string::HeapBuilder>& keysToAdd,
                      std::vector<key_string::HeapBuilder>* out,
                      key_string::Version keyStringVersion,
                      SortedDataIndexAccessMethod::GetKeysContext context,
                      Ordering ordering,
                      size_t maxKeys) {
    bool foundIndexedArrayValue = false;
    if (0 == elements.size()) {
        // Missing fields are indexed as null.
        appendToS2Keys(keysToAdd,
                       out,
                       keyStringVersion,
                       context,
                       ordering,
                       maxKeys,
                       [](key_string::HeapBuilder& ks) { ks.appendNull(); });
    } else {
        for (BSONElementSet::iterator i = elements.begin(); i != elements.end(); ++i) {
            const bool thisElemIsArray = getS2OneLiteralKey(
                *i, collator, keysToAdd, out, keyStringVersion, context, ordering, maxKeys);
            foundIndexedArrayValue = foundIndexedArrayValue || thisElemIsArray;
        }
    }
    return foundIndexedArrayValue;
}

}  // namespace

void getS2Keys(SharedBufferFragmentBuilder& pooledBufferBuilder,
               const BSONObj& obj,
               const BSONObj& keyPattern,
               const S2IndexingParams& params,
               KeyStringSet* keys,
               MultikeyPaths* multikeyPaths,
               key_string::Version keyStringVersion,
               SortedDataIndexAccessMethod::GetKeysContext context,
               Ordering ordering,
               const boost::optional<RecordId>& id) {

    std::vector<key_string::HeapBuilder> keysToAdd;

    // Does one of our documents have a geo field?
    bool haveGeoField = false;

    if (multikeyPaths) {
        tassert(9911909, "", multikeyPaths->empty());
        multikeyPaths->resize(keyPattern.nFields());
    }

    size_t posInIdx = 0;

    try {
        size_t maxNumKeys = gIndexMaxNumGeneratedKeysPerDocument;
        // We output keys in the same order as the fields we index.
        for (const auto& keyElem : keyPattern) {
            // First, we get the keys that this field adds.  Either they're added literally from
            // the value of the field, or they're transformed if the field is geo.
            BSONElementSet fieldElements;
            const bool expandArrayOnTrailingField = false;
            MultikeyComponents* arrayComponents =
                multikeyPaths ? &(*multikeyPaths)[posInIdx] : nullptr;

            // Trailing array values aren't being expanded, so we still need to determine whether
            // the last component of the indexed path 'keyElem.fieldName()' causes the index to be
            // multikey. We say that it does if
            //   (a) the last component of the indexed path ever refers to an array value
            //   (regardless of
            //       the number of array elements)
            //   (b) the last component of the indexed path ever refers to GeoJSON data that
            //   requires
            //       multiple cells for its covering.
            bool lastPathComponentCausesIndexToBeMultikey;
            std::vector<key_string::HeapBuilder> updatedKeysToAdd;

            if (IndexNames::GEO_2DSPHERE_BUCKET == keyElem.str()) {
                auto elementStorage =
                    timeseries::dotted_path_support::extractAllElementsAlongBucketPath(
                        obj,
                        keyElem.fieldName(),
                        fieldElements,
                        expandArrayOnTrailingField,
                        arrayComponents);

                // null, undefined, {} and [] should all behave like there is no geo field. So we
                // look for these cases and ignore those measurements if we find them.
                for (auto it = fieldElements.begin(); it != fieldElements.end();) {
                    decltype(it) next = std::next(it);
                    if (it->isNull() || BSONType::undefined == it->type() ||
                        (it->isABSONObj() && 0 == it->Obj().nFields())) {
                        fieldElements.erase(it);
                    }
                    it = next;
                }

                // 2dsphere indices require that at least one geo field to be present in a
                // document in order to index it.
                if (fieldElements.size() > 0) {
                    haveGeoField = true;
                }

                lastPathComponentCausesIndexToBeMultikey = getS2BucketGeoKeys(obj,
                                                                              fieldElements,
                                                                              params,
                                                                              keysToAdd,
                                                                              &updatedKeysToAdd,
                                                                              keyStringVersion,
                                                                              context,
                                                                              ordering,
                                                                              maxNumKeys);
            } else {
                mdps::extractAllElementsAlongPath(obj,
                                                  keyElem.fieldName(),
                                                  fieldElements,
                                                  expandArrayOnTrailingField,
                                                  arrayComponents);

                if (IndexNames::GEO_2DSPHERE == keyElem.str()) {
                    if (params.indexVersion >= S2_INDEX_VERSION_2) {
                        // For >= V2,
                        // geo: null,
                        // geo: undefined
                        // geo: []
                        // should all behave like there is no geo field.  So we look for these cases
                        // and throw out the field elements if we find them.
                        if (1 == fieldElements.size()) {
                            BSONElement elt = *fieldElements.begin();
                            // Get the :null and :undefined cases.
                            if (elt.isNull() || BSONType::undefined == elt.type()) {
                                fieldElements.clear();
                            } else if (elt.isABSONObj()) {
                                // And this is the :[] case.
                                if (0 == elt.Obj().nFields()) {
                                    fieldElements.clear();
                                }
                            }
                        }

                        // >= V2 2dsphere indices require that at least one geo field to be present
                        // in a document in order to index it.
                        if (fieldElements.size() > 0) {
                            haveGeoField = true;
                        }
                    }

                    lastPathComponentCausesIndexToBeMultikey = getS2GeoKeys(obj,
                                                                            fieldElements,
                                                                            params,
                                                                            keysToAdd,
                                                                            &updatedKeysToAdd,
                                                                            keyStringVersion,
                                                                            context,
                                                                            ordering,
                                                                            maxNumKeys);
                } else {
                    lastPathComponentCausesIndexToBeMultikey = getS2LiteralKeys(fieldElements,
                                                                                params.collator,
                                                                                keysToAdd,
                                                                                &updatedKeysToAdd,
                                                                                keyStringVersion,
                                                                                context,
                                                                                ordering,
                                                                                maxNumKeys);
                }
            }


            // We expect there to be the missing field element present in the keys if data is
            // missing.  So, this should be non-empty.
            tassert(9911906, "", !updatedKeysToAdd.empty());

            if (multikeyPaths && lastPathComponentCausesIndexToBeMultikey) {
                const size_t pathLengthOfThisField =
                    FieldRef{keyElem.fieldNameStringData()}.numParts();
                tassert(9911907, "", pathLengthOfThisField > 0);
                (*multikeyPaths)[posInIdx].insert(pathLengthOfThisField - 1);
            }

            keysToAdd = std::move(updatedKeysToAdd);
            ++posInIdx;
        }
    } catch (const MaxKeysExceededException&) {
        uasserted(
            ErrorCodes::CannotBuildIndexKeys,
            str::stream() << "Insert of geo object exceeded maximum number of generated keys: "
                          << redact(obj));
    }

    // Make sure that if we're >= V2 there's at least one geo field present in the doc.
    if (params.indexVersion >= S2_INDEX_VERSION_2) {
        if (!haveGeoField) {
            return;
        }
    }

    tassert(9911908, "", keys->empty());
    auto keysSequence = keys->extract_sequence();
    for (auto& ks : keysToAdd) {
        if (id) {
            ks.appendRecordId(*id);
        }
        keysSequence.push_back(ks.release());
    }
    keys->adopt_sequence(std::move(keysSequence));
}
}  // namespace mongo::index2dsphere
