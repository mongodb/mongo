// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/modules.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <string>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace fts {

class FTSSpec;

class FTSIndexFormat {
public:
    static void getKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                        const FTSSpec& spec,
                        const BSONObj& document,
                        KeyStringSet* keys,
                        key_string::Version keyStringVersion,
                        Ordering ordering,
                        const boost::optional<RecordId>& id = boost::none);

    /**
     * Helper method to get return entry from the FTSIndex as a BSONObj
     * @param weight, the weight of the term in the entry
     * @param term, the std::string term in the entry
     * @param indexPrefix, the fields that go in the index first
     * @param textIndexVersion, index version. affects key format.
     */
    static BSONObj getIndexKey(double weight,
                               const std::string& term,
                               const BSONObj& indexPrefix,
                               TextIndexVersion textIndexVersion);

    /**
     * Legacy version of getKeys that uses pre-SERVER-76875 dotted path extraction.
     *
     * This function generates keys using the legacy behavior where fields with embedded
     * dots are checked before traversing nested objects. Used only for validation to
     * detect TEXT_INDEX_VERSION_3 indexes need to be rebuilt.
     *
     * Should not be used for normal index operations.
     */
    static void getKeysLegacy_forValidationOnly(SharedBufferFragmentBuilder& pooledBufferBuilder,
                                                const FTSSpec& spec,
                                                const BSONObj& obj,
                                                KeyStringSet* keys,
                                                key_string::Version keyStringVersion,
                                                Ordering ordering,
                                                const boost::optional<RecordId>& id);

private:
    /**
     * Helper method to get return entry from the FTSIndex as a BSONObj.
     * 'b' is a reference to the BSONOBjBuilder.
     * 'weight' is the weight of the term in the entry.
     * 'term' is the std::string term in the entry.
     * 'textIndexVersion' is index version, affects key format.
     */
    static void _appendIndexKey(BSONObjBuilder& b,
                                double weight,
                                const std::string& term,
                                TextIndexVersion textIndexVersion);

    /**
     * Helper method to get return entry from the FTSIndex as a BSONObj.
     * 'keyString' is a reference to the KeyString builder.
     * 'weight' is the weight of the term in the entry.
     * 'term' is the std::string term in the entry.
     * 'textIndexVersion' is index version, affects key format.
     */
    template <typename KeyStringBuilder>
    static void _appendIndexKey(KeyStringBuilder& keyString,
                                double weight,
                                const std::string& term,
                                TextIndexVersion textIndexVersion);

    /**
     * Common implementation for getKeys and getKeysLegacy.
     * Extracts FTS index keys using the provided extraction function for non-FTS fields.
     */
    template <typename ExtractorFunction>
    static void _getKeysImpl(SharedBufferFragmentBuilder& pooledBufferBuilder,
                             const FTSSpec& spec,
                             const BSONObj& obj,
                             KeyStringSet* keys,
                             key_string::Version keyStringVersion,
                             Ordering ordering,
                             const boost::optional<RecordId>& id,
                             ExtractorFunction extractFn);
};
}  // namespace fts
}  // namespace mongo
