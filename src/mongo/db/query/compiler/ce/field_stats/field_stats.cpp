// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/field_stats/field_stats.h"

#include "mongo/db/field_ref.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>

namespace mongo::ce {
namespace {

// Backslash-escapes '|' (the _id separator) and '\' itself, so that field paths containing
// either character cannot collide with another path list in the _id string.
std::string escapeFieldPath(const std::string& path) {
    std::string escaped;
    escaped.reserve(path.size());
    for (const char c : path) {
        if (c == '\\' || c == '|') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    return escaped;
}

}  // namespace

std::string makeFieldStatsId(const UUID& collectionUuid, std::vector<std::string> fieldPaths) {
    tassert(13175805, "A field-stats id requires at least one field path", !fieldPaths.empty());
    std::sort(fieldPaths.begin(), fieldPaths.end());

    str::stream id;
    id << kFieldStatsSchemaVersion << "|" << collectionUuid.toString();
    for (const auto& path : fieldPaths) {
        id << "|" << escapeFieldPath(path);
    }
    return id;
}

void validateFieldStatsPath(std::string_view path) {
    const FieldRef fieldRef(path);
    uassert(6799703, "Key path is empty", !fieldRef.empty());

    const auto numericPathComponents = fieldRef.getNumericPathComponents(0);
    uassert(6799704,
            str::stream() << "Key path contains numeric component "
                          << fieldRef.getPart(*(numericPathComponents.begin())),
            numericPathComponents.empty());

    for (size_t i = 0; i < fieldRef.numParts(); ++i) {
        uassertStatusOK(FieldPath::validateFieldName(fieldRef.getPart(i)));
    }
}

}  // namespace mongo::ce
