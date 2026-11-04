// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/stats/scalar_field_values.h"

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/util/assert_util.h"

namespace mongo::stats {
namespace {

using sbe::value::TagValueView;
using sbe::value::TypeTags;

TagValueView extractScalarField(TagValueView value, const FieldPath& path) {
    for (size_t i = 0;; ++i) {
        uassert(13175701,
                "Scalar field extraction does not support array values",
                !sbe::value::isArray(value.tag));
        if (i == path.getPathLength()) {
            return value;
        }
        if (!sbe::value::isObject(value.tag)) {
            return TagValueView::nothing();
        }
        const auto field = path.getFieldName(i);
        value = value.tag == TypeTags::bsonObject
            ? sbe::bson::getField(sbe::value::getRawPointerView(value.value), field)
            : sbe::value::getObjectView(value.value)->getField(field);
    }
}

}  // namespace

absl::InlinedVector<TagValueView, 3> extractScalarFieldValues(TagValueView doc,
                                                              std::span<const FieldPath> paths) {
    absl::InlinedVector<TagValueView, 3> values;
    values.reserve(paths.size());
    for (const auto& path : paths) {
        values.push_back(extractScalarField(doc, path));
    }
    return values;
}

}  // namespace mongo::stats
