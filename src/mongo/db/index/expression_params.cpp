/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/db/index/expression_params.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/2d_common.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/index_names.h"
#include "mongo/util/mongoutils/str.h"
#include "third_party/s2/s2.h"

namespace mongo {

using mongoutils::str::stream;

void ExpressionParams::parseTwoDParams(const BSONObj& infoObj, TwoDIndexingParams* out) {
    BSONObjIterator i(infoObj.getObjectField("key"));

    while (i.more()) {
        BSONElement e = i.next();
        if (e.type() == String && IndexNames::GEO_2D == e.valuestr()) {
            uassert(16800, "can't have 2 geo fields", out->geo.size() == 0);
            uassert(16801, "2d has to be first in index", out->other.size() == 0);
            out->geo = e.fieldName();
        } else {
            int order = 1;
            if (e.isNumber()) {
                order = static_cast<int>(e.Number());
            }
            out->other.push_back(std::make_pair(e.fieldName(), order));
        }
    }

    uassert(16802, "no geo field specified", out->geo.size());

    GeoHashConverter::Parameters hashParams;
    Status paramStatus = GeoHashConverter::parseParameters(infoObj, &hashParams);
    uassertStatusOK(paramStatus);

    out->geoHashConverter.reset(new GeoHashConverter(hashParams));
}

void ExpressionParams::parseHashParams(const BSONObj& infoObj,
                                       HashSeed* seedOut,
                                       int* versionOut,
                                       std::string* fieldOut) {
    // Default _seed to DEFAULT_HASH_SEED if "seed" is not included in the index spec
    // or if the value of "seed" is not a number

    // *** WARNING ***
    // Choosing non-default seeds will invalidate hashed sharding
    // Changing the seed default will break existing indexes and sharded collections
    if (infoObj["seed"].eoo()) {
        *seedOut = BSONElementHasher::DEFAULT_HASH_SEED;
    } else {
        *seedOut = infoObj["seed"].numberInt();
    }

    // In case we have hashed indexes based on other hash functions in the future, we store
    // a hashVersion number. If hashVersion changes, "makeSingleHashKey" will need to change
    // accordingly.  Defaults to 0 if "hashVersion" is not included in the index spec or if
    // the value of "hashversion" is not a number
    *versionOut = infoObj["hashVersion"].numberInt();

    // Get the hashfield name
    BSONElement firstElt = infoObj.getObjectField("key").firstElement();
    massert(16765, "error: no hashed index field", firstElt.str().compare(IndexNames::HASHED) == 0);
    *fieldOut = firstElt.fieldName();
}

void ExpressionParams::parseHaystackParams(const BSONObj& infoObj,
                                           std::string* geoFieldOut,
                                           std::vector<std::string>* otherFieldsOut,
                                           double* bucketSizeOut) {
    BSONElement e = infoObj["bucketSize"];
    uassert(16777, "need bucketSize", e.isNumber());
    *bucketSizeOut = e.numberDouble();
    uassert(16769, "bucketSize cannot be zero", *bucketSizeOut != 0.0);

    // Example:
    // db.foo.ensureIndex({ pos : "geoHaystack", type : 1 }, { bucketSize : 1 })
    BSONObjIterator i(infoObj.getObjectField("key"));
    while (i.more()) {
        BSONElement e = i.next();
        if (e.type() == String && IndexNames::GEO_HAYSTACK == e.valuestr()) {
            uassert(16770, "can't have more than one geo field", geoFieldOut->size() == 0);
            uassert(16771, "the geo field has to be first in index", otherFieldsOut->size() == 0);
            *geoFieldOut = e.fieldName();
        } else {
            uassert(16772,
                    "geoSearch can only have 1 non-geo field for now",
                    otherFieldsOut->size() == 0);
            otherFieldsOut->push_back(e.fieldName());
        }
    }
}

void ExpressionParams::initialize2dsphereParams(const BSONObj& infoObj,
                                                const CollatorInterface* collator,
                                                S2IndexingParams* out) {
    // Set up basic params.
    out->collator = collator;
    out->maxKeysPerInsert = 200;

    // Near distances are specified in meters...sometimes.
    out->radius = kRadiusOfEarthInMeters;

    static const std::string kIndexVersionFieldName("2dsphereIndexVersion");
    static const std::string kFinestIndexedLevel("finestIndexedLevel");
    static const std::string kCoarsestIndexedLevel("coarsestIndexedLevel");

    long long indexVersion;

    // Determine which version of this index we're using.  If none was set in the descriptor,
    // assume S2_INDEX_VERSION_1 (alas, the first version predates the existence of the version
    // field).
    Status status = bsonExtractIntegerFieldWithDefault(
        infoObj, kIndexVersionFieldName, S2_INDEX_VERSION_1, &indexVersion);
    uassertStatusOK(status);

    out->indexVersion = static_cast<S2IndexVersion>(indexVersion);

    // Note: In version > 2, these levels are for non-points.
    // Points are always indexed to the finest level.
    // Default levels were optimized for buildings and state regions
    long long defaultFinestIndexedLevel = S2::kAvgEdge.GetClosestLevel(110.0 / out->radius);
    long long defaultCoarsestIndexedLevel =
        S2::kAvgEdge.GetClosestLevel(2000 * 1000.0 / out->radius);
    long long defaultMaxCellsInCovering = 20;

    if (out->indexVersion <= S2_INDEX_VERSION_2) {
        defaultFinestIndexedLevel = S2::kAvgEdge.GetClosestLevel(500.0 / out->radius);
        defaultCoarsestIndexedLevel = S2::kAvgEdge.GetClosestLevel(100.0 * 1000 / out->radius);
        defaultMaxCellsInCovering = 50;
    }

    long long finestIndexedLevel, coarsestIndexedLevel, maxCellsInCovering;

    status = bsonExtractIntegerFieldWithDefault(
        infoObj, "finestIndexedLevel", defaultFinestIndexedLevel, &finestIndexedLevel);
    uassertStatusOK(status);

    status = bsonExtractIntegerFieldWithDefault(
        infoObj, "coarsestIndexedLevel", defaultCoarsestIndexedLevel, &coarsestIndexedLevel);
    uassertStatusOK(status);

    status = bsonExtractIntegerFieldWithDefault(
        infoObj, "maxCellsInCovering", defaultMaxCellsInCovering, &maxCellsInCovering);
    uassertStatusOK(status);

    // This is advisory.
    out->maxCellsInCovering = maxCellsInCovering;

    // These are not advisory.
    out->finestIndexedLevel = finestIndexedLevel;
    out->coarsestIndexedLevel = coarsestIndexedLevel;

    uassert(16747, "coarsestIndexedLevel must be >= 0", out->coarsestIndexedLevel >= 0);
    uassert(16748, "finestIndexedLevel must be <= 30", out->finestIndexedLevel <= 30);
    uassert(16749,
            "finestIndexedLevel must be >= coarsestIndexedLevel",
            out->finestIndexedLevel >= out->coarsestIndexedLevel);


    massert(17395,
            stream() << "unsupported geo index version { " << kIndexVersionFieldName << " : "
                     << out->indexVersion
                     << " }, only support versions: ["
                     << S2_INDEX_VERSION_1
                     << ","
                     << S2_INDEX_VERSION_2
                     << ","
                     << S2_INDEX_VERSION_3
                     << "]",
            out->indexVersion == S2_INDEX_VERSION_3 || out->indexVersion == S2_INDEX_VERSION_2 ||
                out->indexVersion == S2_INDEX_VERSION_1);
}
}  // namespace mongo
