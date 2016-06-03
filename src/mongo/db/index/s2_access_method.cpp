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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/db/index/s2_access_method.h"

#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/geo/geoconstants.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/log.h"

namespace mongo {

static const string kIndexVersionFieldName("2dsphereIndexVersion");

S2AccessMethod::S2AccessMethod(IndexCatalogEntry* btreeState, SortedDataInterface* btree)
    : IndexAccessMethod(btreeState, btree) {
    const IndexDescriptor* descriptor = btreeState->descriptor();

    ExpressionParams::initialize2dsphereParams(
        descriptor->infoObj(), btreeState->getCollator(), &_params);

    int geoFields = 0;

    // Categorize the fields we're indexing and make sure we have a geo field.
    BSONObjIterator i(descriptor->keyPattern());
    while (i.more()) {
        BSONElement e = i.next();
        if (e.type() == String && IndexNames::GEO_2DSPHERE == e.String()) {
            ++geoFields;
        } else {
            // We check for numeric in 2d, so that's the check here
            uassert(16823,
                    (string) "Cannot use " + IndexNames::GEO_2DSPHERE +
                        " index with other special index types: " + e.toString(),
                    e.isNumber());
        }
    }

    uassert(16750,
            "Expect at least one geo field, spec=" + descriptor->keyPattern().toString(),
            geoFields >= 1);

    if (descriptor->isSparse()) {
        warning() << "Sparse option ignored for index spec " << descriptor->keyPattern().toString()
                  << "\n";
    }
}

// static
StatusWith<BSONObj> S2AccessMethod::fixSpec(const BSONObj& specObj) {
    // If the spec object has the field "2dsphereIndexVersion", validate it.  If it doesn't, add
    // {2dsphereIndexVersion: 3}, which is the default for newly-built indexes.

    BSONElement indexVersionElt = specObj[kIndexVersionFieldName];
    if (indexVersionElt.eoo()) {
        BSONObjBuilder bob;
        bob.appendElements(specObj);
        bob.append(kIndexVersionFieldName, S2_INDEX_VERSION_3);
        return bob.obj();
    }

    if (!indexVersionElt.isNumber()) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "Invalid type for geo index version { " << kIndexVersionFieldName
                              << " : "
                              << indexVersionElt
                              << " }, only versions: ["
                              << S2_INDEX_VERSION_1
                              << ","
                              << S2_INDEX_VERSION_2
                              << ","
                              << S2_INDEX_VERSION_3
                              << "] are supported"};
    }

    if (indexVersionElt.type() == BSONType::NumberDouble &&
        !std::isnormal(indexVersionElt.numberDouble())) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "Invalid value for geo index version { " << kIndexVersionFieldName
                              << " : "
                              << indexVersionElt
                              << " }, only versions: ["
                              << S2_INDEX_VERSION_1
                              << ","
                              << S2_INDEX_VERSION_2
                              << ","
                              << S2_INDEX_VERSION_3
                              << "] are supported"};
    }

    const auto indexVersion = indexVersionElt.numberLong();
    if (indexVersion != S2_INDEX_VERSION_1 && indexVersion != S2_INDEX_VERSION_2 &&
        indexVersion != S2_INDEX_VERSION_3) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "unsupported geo index version { " << kIndexVersionFieldName
                              << " : "
                              << indexVersionElt
                              << " }, only versions: ["
                              << S2_INDEX_VERSION_1
                              << ","
                              << S2_INDEX_VERSION_2
                              << ","
                              << S2_INDEX_VERSION_3
                              << "] are supported"};
    }

    return specObj;
}

void S2AccessMethod::getKeys(const BSONObj& obj,
                             BSONObjSet* keys,
                             MultikeyPaths* multikeyPaths) const {
    ExpressionKeysPrivate::getS2Keys(obj, _descriptor->keyPattern(), _params, keys, multikeyPaths);
}

}  // namespace mongo
