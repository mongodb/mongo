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

#include "mongo/db/fts/fts_index_format.h"
#include "mongo/db/geo/s2common.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index/2d_common.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    void getKeysForUpgradeChecking(const BSONObj& infoObj,
                                   const BSONObj& doc,
                                   BSONObjSet* keys) {

        BSONObj keyPattern = infoObj.getObjectField("key");

        string type = IndexNames::findPluginName(keyPattern);

        if (IndexNames::GEO_2D == type) {
            TwoDIndexingParams params;
            ExpressionParams::parseTwoDParams(infoObj, &params);
            ExpressionKeysPrivate::get2DKeys(doc, params, keys, NULL);
        }
        else if (IndexNames::GEO_HAYSTACK == type) {
            string geoField;
            vector<string> otherFields;
            double bucketSize;
            ExpressionParams::parseHaystackParams(infoObj, &geoField, &otherFields, &bucketSize);
            ExpressionKeysPrivate::getHaystackKeys(doc, geoField, otherFields, bucketSize, keys);
        }
        else if (IndexNames::GEO_2DSPHERE == type) {
            S2IndexingParams params;
            ExpressionParams::parse2dsphereParams(infoObj, &params);
            ExpressionKeysPrivate::getS2Keys(doc, keyPattern, params, keys);
        }
        else if (IndexNames::TEXT == type) {
            fts::FTSSpec spec(infoObj);
            ExpressionKeysPrivate::getFTSKeys(doc, spec, keys);
        }
        else if (IndexNames::HASHED == type) {
            HashSeed seed;
            int version;
            string field;
            ExpressionParams::parseHashParams(infoObj, &seed, &version, &field);
            ExpressionKeysPrivate::getHashKeys(doc, field, seed, version, infoObj["sparse"].trueValue(), keys);
        }
        else {
            invariant(IndexNames::BTREE == type);

            std::vector<const char *> fieldNames;
            std::vector<BSONElement> fixed;
            BSONObjIterator keyIt(keyPattern);
            while (keyIt.more()) {
                BSONElement patternElt = keyIt.next();
                fieldNames.push_back(patternElt.fieldName());
                fixed.push_back(BSONElement());
            }

            // XXX: do we care about version
            BtreeKeyGeneratorV1 keyGen(fieldNames, fixed, infoObj["sparse"].trueValue());

            keyGen.getKeys(doc, keys);
        }
    }

}  // namespace mongo
