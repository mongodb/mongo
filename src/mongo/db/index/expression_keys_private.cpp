/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/db/index/expression_keys_private.h"

#include <absl/container/node_hash_map.h>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/fts/fts_index_format.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_dotted_path_support.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/container/flat_set.hpp>
#include <boost/container/vector.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex
namespace mongo {
namespace dps = ::mongo::bson;

// static
void ExpressionKeysPrivate::validateDocumentCommon(const CollectionPtr& collection,
                                                   const BSONObj& obj,
                                                   const BSONObj& keyPattern) {
    // If we have a timeseries collection, check that indexed metric fields do not have expanded
    // array values
    if (auto tsOptions = collection->getTimeseriesOptions()) {
        // Each user metric field will be included twice, as both control.min.<field> and
        // control.max.<field>, so we'll want to keep track that we've checked data.<field> to avoid
        // scanning it twice. The time field can be excluded as it is guaranteed to be a date at
        // insertion time.
        StringSet userFieldsChecked;

        for (const auto& keyElem : keyPattern) {
            if (keyElem.isNumber()) {
                StringData field = keyElem.fieldName();
                StringData userField;

                if (field.starts_with(timeseries::kControlMaxFieldNamePrefix)) {
                    userField = field.substr(timeseries::kControlMaxFieldNamePrefix.size());
                } else if (field.starts_with(timeseries::kControlMinFieldNamePrefix)) {
                    userField = field.substr(timeseries::kControlMinFieldNamePrefix.size());
                }

                if (!userField.empty() && userField == tsOptions->getTimeField()) {
                    // Exclude checking the time field. Time values are explicitly dates and not
                    // arrays.
                    continue;
                }

                if (!userField.empty() && !userFieldsChecked.contains(userField)) {
                    namespace tdps = timeseries::dotted_path_support;
                    // We are in fact dealing with a metric field. First let's check the min and max
                    // values to see if we can conclude that there are no arrays present in the
                    // data.
                    auto decision = tdps::fieldContainsArrayData(obj, userField);
                    if (decision != tdps::Decision::No) {
                        // Go ahead and look closer
                        uassert(5930501,
                                str::stream()
                                    << "Indexed measurement field contains an array value",
                                decision == tdps::Decision::Maybe &&
                                    !tdps::haveArrayAlongBucketDataPath(
                                        obj,
                                        std::string(timeseries::kDataFieldNamePrefix) + userField));
                    }
                    userFieldsChecked.emplace(userField);
                }
            }
        }
    }
}

// static
void ExpressionKeysPrivate::getFTSKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                                       const BSONObj& obj,
                                       const fts::FTSSpec& ftsSpec,
                                       KeyStringSet* keys,
                                       key_string::Version keyStringVersion,
                                       Ordering ordering,
                                       const boost::optional<RecordId>& id) {
    fts::FTSIndexFormat::getKeys(
        pooledBufferBuilder, ftsSpec, obj, keys, keyStringVersion, ordering, id);
}

// static
void ExpressionKeysPrivate::getHashKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                                        const BSONObj& obj,
                                        const BSONObj& keyPattern,
                                        int hashVersion,
                                        bool isSparse,
                                        const CollatorInterface* collator,
                                        KeyStringSet* keys,
                                        key_string::Version keyStringVersion,
                                        Ordering ordering,
                                        bool ignoreArraysAlongPath,
                                        const boost::optional<RecordId>& id) {
    static const BSONObj nullObj = BSON("" << BSONNULL);
    auto hasFieldValue = false;
    key_string::PooledBuilder keyString(pooledBufferBuilder, keyStringVersion, ordering);
    for (auto&& indexEntry : keyPattern) {
        auto indexPath = indexEntry.fieldNameStringData();
        auto* cstr = indexPath.data();
        auto fieldVal = dps::extractElementAtOrArrayAlongDottedPath(obj, cstr);

        // If we hit an array while traversing the path, 'cstr' will point to the path component
        // immediately following the array, or the null termination byte if the terminal path
        // component was an array. In the latter case, 'remainingPath' will be empty.
        auto remainingPath = StringData(cstr);

        // If 'ignoreArraysAlongPath' is set, we want to use the behaviour prior to SERVER-44050,
        // which is to allow arrays along the field path (except the terminal path). This is done so
        // that the document keys inserted prior to SERVER-44050 can be deleted or updated after the
        // upgrade, allowing users to recover from the possible index corruption. The old behaviour
        // before SERVER-44050 was to store 'null' index key if we encountered an array along the
        // index field path. We will use the same logic in the context of removing index keys.
        if (ignoreArraysAlongPath && fieldVal.type() == BSONType::array && !remainingPath.empty()) {
            fieldVal = nullObj.firstElement();
        }

        // Otherwise, throw if an array was encountered at any point along the path.
        uassert(16766,
                str::stream() << "Error: hashed indexes do not currently support array values. "
                                 "Found array at path: "
                              << indexPath.substr(0,
                                                  indexPath.size() - remainingPath.size() -
                                                      !remainingPath.empty()),
                fieldVal.type() != BSONType::array);

        BSONObj fieldValObj;
        if (fieldVal.eoo()) {
            fieldVal = nullObj.firstElement();
        } else {
            BSONObjBuilder bob;
            CollationIndexKey::collationAwareIndexKeyAppend(fieldVal, collator, &bob);
            fieldValObj = bob.obj();
            fieldVal = fieldValObj.firstElement();
            hasFieldValue = true;
        }

        if (indexEntry.isNumber()) {
            keyString.appendBSONElement(fieldVal);
        } else {
            keyString.appendNumberLong(makeSingleHashKey(fieldVal, hashVersion));
        }
    }
    if (isSparse && !hasFieldValue) {
        return;
    }
    if (id) {
        keyString.appendRecordId(*id);
    }
    keys->insert(keyString.release());
}

// static
long long int ExpressionKeysPrivate::makeSingleHashKey(const BSONElement& e, int v) {
    // *** WARNING ***
    // Changing the seed default will break existing indexes and sharded collections
    massert(16767, "Only HashVersion 0 has been defined", v == 0);
    return BSONElementHasher::hash64(e, BSONElementHasher::DEFAULT_HASH_SEED);
}

}  // namespace mongo
