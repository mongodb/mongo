// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index/wildcard_metadata_key.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

FieldRef decodeWildcardMultikeyMetadataPath(const BSONObj& keyBson) {
    BSONObjIterator iter(keyBson);
    while (iter.more()) {
        const auto elem = iter.next();
        if (elem.type() == BSONType::minKey) {
            continue;
        }
        tassert(7354603,
                "An int value must follow MinKey values in a metadata key of a wildcard index.",
                elem.isNumber());
        tassert(7354604,
                "The int value '1' must follow MinKey values in a metadata key of a wildcard "
                "index.",
                elem.numberInt() == 1);
        tassert(7354605,
                "A string value must follow an int value in a metadata key of a wildcard index",
                iter.more());
        const auto nextElem = iter.next();
        tassert(7354606,
                "A string value must follow an int value in a metadata key of a wildcard index",
                nextElem.type() == BSONType::string);
        return FieldRef(nextElem.valueStringData());
    }
    tasserted(7354607,
              str::stream() << "Unexpected format of a metadata key of a wildcard index: "
                            << keyBson);
}

std::set<FieldRef> extractWildcardMultikeyPathsFromMetadataKeys(const KeyStringSet& metadataKeys,
                                                                const Ordering& ordering) {
    std::set<FieldRef> paths;
    for (const auto& key : metadataKeys) {
        BSONObj keyBson = key_string::toBsonSafe(key.getView(), ordering, key.getTypeBits());
        paths.insert(decodeWildcardMultikeyMetadataPath(keyBson));
    }
    return paths;
}

}  // namespace mongo
