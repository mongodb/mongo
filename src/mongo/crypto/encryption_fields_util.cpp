// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

boost::optional<QueryTypeEnum> findStringSearchQueryType(const EncryptedField& field) {
    boost::optional<QueryTypeEnum> result;
    visitQueryTypeConfigs(field, [&](const EncryptedField&, const QueryTypeConfig& qtc) {
        if (isFLE2TextQueryType(qtc.getQueryType())) {
            result = qtc.getQueryType();
            return true;
        }
        return false;
    });
    return result;
}

}  // namespace mongo
