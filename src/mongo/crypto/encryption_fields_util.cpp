/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/crypto/encryption_fields_util.h"

#include <boost/container/small_vector.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include <algorithm>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

boost::optional<EncryptedFieldMatchResult> findMatchingEncryptedField(
    const FieldRef& key, const std::vector<FieldRef>& encryptedFields) {
    auto itr = std::find_if(encryptedFields.begin(),
                            encryptedFields.end(),
                            [&key](const auto& field) { return key.fullyOverlapsWith(field); });
    if (itr == encryptedFields.end()) {
        return boost::none;
    }
    return {{*itr, key.numParts() <= itr->numParts()}};
}

bool visitQueryTypeConfigs(const EncryptedField& field,
                           const QueryTypeConfigVisitor& visitOne,
                           const UnindexedEncryptedFieldVisitor& onEmptyField) {
    if (!field.getQueries()) {
        if (onEmptyField) {
            return onEmptyField(field);
        }
        return false;
    }

    return visit(OverloadedVisitor{[&](QueryTypeConfig query) { return visitOne(field, query); },
                                   [&](std::vector<QueryTypeConfig> queries) {
                                       return std::any_of(queries.cbegin(),
                                                          queries.cend(),
                                                          [&](const QueryTypeConfig& qtc) {
                                                              return visitOne(field, qtc);
                                                          });
                                   }},
                 field.getQueries().get());
}

bool visitQueryTypeConfigs(const EncryptedFieldConfig& efc,
                           const QueryTypeConfigVisitor& visitOne,
                           const UnindexedEncryptedFieldVisitor& onEmptyField) {
    for (const auto& field : efc.getFields()) {
        if (visitQueryTypeConfigs(field, visitOne, onEmptyField)) {
            return true;
        }
    }
    return false;
}

}  // namespace mongo
