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

#include "mongo/base/string_data.h"

namespace mongo {

static inline constexpr StringData kUpdateOplogEntryVersionFieldName = "$v"_sd;

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
    // were record in oplog entries. Oplog entries of this form are no longer supported, but the
    // user facing modifier-style update system remains. When a single update adds
    // multiple fields, those fields are added in lexicographic order by field name. This system
    // introduces support for arrayFilters and $[] syntax.
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
