/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/index/external_key_generator.h"

#include <cmath>
#include <string>

#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index/2d_common.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"

namespace mongo {

namespace {
void getKeysForUpgradeChecking(const BSONObj& infoObj, const BSONObj& doc, BSONObjSet* keys) {
    BSONObj keyPattern = infoObj.getObjectField("key");

    std::string type = IndexNames::findPluginName(keyPattern);

    if (IndexNames::GEO_2D == type) {
        TwoDIndexingParams params;
        ExpressionParams::parseTwoDParams(infoObj, &params);
        ExpressionKeysPrivate::get2DKeys(doc, params, keys, NULL);
    } else if (IndexNames::GEO_HAYSTACK == type) {
        std::string geoField;
        std::vector<std::string> otherFields;
        double bucketSize;
        ExpressionParams::parseHaystackParams(infoObj, &geoField, &otherFields, &bucketSize);
        ExpressionKeysPrivate::getHaystackKeys(doc, geoField, otherFields, bucketSize, keys);
    } else if (IndexNames::GEO_2DSPHERE == type) {
        S2IndexingParams params;
        // TODO SERVER-22251: If the index has a collator, it should be passed here, or the keys
        // generated will be wrong.
        CollatorInterface* collator = nullptr;
        ExpressionParams::initialize2dsphereParams(infoObj, collator, &params);

        // There's no need to compute the prefixes of the indexed fields that cause the index to be
        // multikey when checking if any index key is too large.
        MultikeyPaths* multikeyPaths = nullptr;
        ExpressionKeysPrivate::getS2Keys(doc, keyPattern, params, keys, multikeyPaths);
    } else if (IndexNames::TEXT == type) {
        fts::FTSSpec spec(infoObj);
        ExpressionKeysPrivate::getFTSKeys(doc, spec, keys);
    } else if (IndexNames::HASHED == type) {
        HashSeed seed;
        int version;
        std::string field;
        ExpressionParams::parseHashParams(infoObj, &seed, &version, &field);
        // TODO SERVER-22251: If the index has a collator, it should be passed here, or the keys
        // generated will be wrong.
        CollatorInterface* collator = nullptr;
        ExpressionKeysPrivate::getHashKeys(
            doc, field, seed, version, infoObj["sparse"].trueValue(), collator, keys);
    } else {
        invariant(IndexNames::BTREE == type);

        std::vector<const char*> fieldNames;
        std::vector<BSONElement> fixed;
        BSONObjIterator keyIt(keyPattern);
        while (keyIt.more()) {
            BSONElement patternElt = keyIt.next();
            fieldNames.push_back(patternElt.fieldName());
            fixed.push_back(BSONElement());
        }

        // XXX: do we care about version
        // TODO: change nullptr to a collator, if a collation spec is given.
        BtreeKeyGeneratorV1 keyGen(fieldNames, fixed, infoObj["sparse"].trueValue(), nullptr);

        // There's no need to compute the prefixes of the indexed fields that cause the index to be
        // multikey when checking if any index key is too large.
        MultikeyPaths* multikeyPaths = nullptr;
        keyGen.getKeys(doc, keys, multikeyPaths);
    }
}

// cloned from key.cpp to build the below set
const int binDataCodeLengths[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 32};
const std::set<int> acceptableBinDataLengths(binDataCodeLengths,
                                             binDataCodeLengths +
                                                 (sizeof(binDataCodeLengths) / sizeof(int)));

// modified version of the KeyV1Owned constructor that returns the would-be-key's datasize()
int keyV1Size(const BSONObj& obj) {
    BSONObj::iterator i(obj);
    int size = 0;
    const int traditionalSize = obj.objsize() + 1;
    while (i.more()) {
        BSONElement e = i.next();
        switch (e.type()) {
            case MinKey:
            case jstNULL:
            case MaxKey:
            case Bool:
                size += 1;
                break;
            case jstOID:
                size += 1;
                size += OID::kOIDSize;
                break;
            case BinData: {
                int t = e.binDataType();
                // 0-7 and 0x80 to 0x87 are supported by Key
                if ((t & 0x78) == 0 && t != ByteArrayDeprecated) {
                    int len;
                    e.binData(len);
                    if (acceptableBinDataLengths.count(len)) {
                        size += 1;
                        size += 1;
                        size += len;
                        break;
                    }
                }
                return traditionalSize;
            }
            case Date:
                size += 1;
                size += sizeof(Date_t);
                break;
            case String: {
                size += 1;
                // note we do not store the terminating null, to save space.
                unsigned x = (unsigned)e.valuestrsize() - 1;
                if (x > 255) {
                    return traditionalSize;
                }
                size += 1;
                size += x;
                break;
            }
            case NumberInt:
                size += 1;
                size += sizeof(double);
                break;
            case NumberLong: {
                long long n = e._numberLong();
                long long m = 2LL << 52;
                if (n >= m || n <= -m) {
                    // can't represent exactly as a double
                    return traditionalSize;
                }
                size += 1;
                size += sizeof(double);
                break;
            }
            case NumberDouble: {
                double d = e._numberDouble();
                if (std::isnan(d)) {
                    return traditionalSize;
                }
                size += 1;
                size += sizeof(double);
                break;
            }
            default:
                // if other types involved, store as traditional BSON
                return traditionalSize;
        }
    }
    return size;
}

}  // namespace

bool isAnyIndexKeyTooLarge(const BSONObj& index, const BSONObj& doc) {
    BSONObjSet keys;
    getKeysForUpgradeChecking(index, doc, &keys);

    int largestKeySize = 0;

    for (BSONObjSet::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        largestKeySize = std::max(largestKeySize, keyV1Size(*it));
    }

    // BtreeData_V1::KeyMax is 1024
    return largestKeySize > 1024;
}

}  // namespace mongo
