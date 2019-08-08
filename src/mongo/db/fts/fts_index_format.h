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

#pragma once

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo {

namespace fts {

class FTSSpec;

class FTSIndexFormat {
public:
    static void getKeys(const FTSSpec& spec,
                        const BSONObj& document,
                        KeyStringSet* keys,
                        KeyString::Version keyStringVersion,
                        Ordering ordering,
                        boost::optional<RecordId> id = boost::none);

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

private:
    /**
     * Helper method to get return entry from the FTSIndex as a BSONObj
     * @param b, reference to the BSONOBjBuilder
     * @param weight, the weight of the term in the entry
     * @param term, the std::string term in the entry
     * @param textIndexVersion, index version. affects key format.
     */
    static void _appendIndexKey(BSONObjBuilder& b,
                                double weight,
                                const std::string& term,
                                TextIndexVersion textIndexVersion);
};
}  // namespace fts
}  // namespace mongo
