/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update_index_data.h"

namespace mongo {
namespace doc_diff {

struct DamagesOutput {
    const BSONObj preImage;
    SharedBuffer damageSource;
    mutablebson::DamageVector damages;
};

/**
 * Applies the diff to 'pre' and returns the post image.
 * Throws if the diff is invalid.
 *
 * 'mustCheckExistenceForInsertOperations' parameter signals whether we must check if an inserted
 * field already exists within a (sub)document. This should generally be set to true, unless the
 * caller has knowledge of the pre-image and the diff, and can guarantee that we will not re-insert
 * anything.
 */
BSONObj applyDiff(const BSONObj& pre, const Diff& diff, bool mustCheckExistenceForInsertOperations);

/**
 * Computes the damage events from the diff for 'pre' and return the pre-image, damage source, and
 * damage vector. The final, 'mustCheckExistenceForInsertOperations' parameter signals whether we
 * must check if an inserted field already exists within a (sub)document. This should generally be
 * set to true, unless the caller has knowledge of the pre-image and the diff, and can guarantee
 * that we will not re-insert anything.
 */
DamagesOutput computeDamages(const BSONObj& pre,
                             const Diff& diff,
                             bool mustCheckExistenceForInsertOperations);
}  // namespace doc_diff
}  // namespace mongo
