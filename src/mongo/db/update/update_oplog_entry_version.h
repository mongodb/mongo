// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

static inline constexpr std::string_view kUpdateOplogEntryVersionFieldName = "$v"sv;

/**
 * There are multiple types of 'u' (update) oplog entries. The type of an entry is indicated using
 * a field called $v.
 *
 * The values in this enum *MUST* not change unless we remove support for a type of update.
 */
enum class UpdateOplogEntryVersion {
    // Ancient update system which was deleted in 4.0. We still reserve its version number.
    kRemovedV0 = 0,

    // The update system introduced in v3.6, and, until 5.1, also served the function of how updates
    // were record in oplog entries. Oplog entries of this form are no longer generated, but the
    // user facing modifier-style update system remains. However, the server is still capable of
    // processing these oplog entries when they come from an applyOps command. When a single update
    // adds multiple fields, those fields are added in lexicographic order by field name. This
    // system introduces support for arrayFilters and $[] syntax.
    kUpdateNodeV1 = 1,

    // Delta style update, introduced in 4.7. When a pipeline based update is executed, the pre and
    // post images are diffed, producing a delta. The delta is recorded in the oplog. On
    // secondaries, the delta is applied to the pre-image to recover the post image.
    //
    // Delta style updates cannot be executed directly by users.
    kDeltaV2 = 2,

    // Must be last.
    kNumVersions
};
}  // namespace mongo
