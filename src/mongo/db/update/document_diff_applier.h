// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/storage/damage_vector.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update_index_data.h"
#include "mongo/util/modules.h"
#include "mongo/util/shared_buffer.h"

namespace mongo {
namespace doc_diff {

struct DamagesOutput {
    const BSONObj preImage;
    SharedBuffer damageSource;
    DamageVector damages;
};

/**
 * Applies the diff to 'pre' and returns the post image.
 * Throws if the diff is invalid.
 *
 * 'mustCheckExistenceForInsertOperations' parameter signals whether we must check if an inserted
 * field already exists within a (sub)document. This should generally be set to true, unless the
 * caller has knowledge of the pre-image and the diff, and can guarantee that we will not re-insert
 * anything.
 *
 * 'verifierFunction' is an optional parameter that, if set, will perform a check on the BSONObj
 * that is created as a result of the application of 'diff' onto 'pre'.
 */
using VerifierFunc [[MONGO_MOD_PUBLIC]] =
    std::function<void(const BSONObj& post, const BSONObj& pre)>;
[[MONGO_MOD_PUBLIC]] BSONObj applyDiff(const BSONObj& pre,
                                       const Diff& diff,
                                       bool mustCheckExistenceForInsertOperations,
                                       VerifierFunc verifierFunction = nullptr);

/**
 * Computes the damage events from the diff for 'pre' and return the pre-image, damage source, and
 * damage vector. The final, 'mustCheckExistenceForInsertOperations' parameter signals whether we
 * must check if an inserted field already exists within a (sub)document. This should generally be
 * set to true, unless the caller has knowledge of the pre-image and the diff, and can guarantee
 * that we will not re-insert anything.
 */
[[MONGO_MOD_PUBLIC]] DamagesOutput computeDamages(const BSONObj& pre,
                                                  const Diff& diff,
                                                  bool mustCheckExistenceForInsertOperations);
}  // namespace doc_diff
}  // namespace mongo
