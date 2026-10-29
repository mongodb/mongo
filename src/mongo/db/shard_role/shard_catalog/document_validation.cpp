// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/document_validation.h"


namespace mongo {
const OperationContext::Decoration<DocumentValidationSettings> DocumentValidationSettings::get =
    OperationContext::declareDecoration<DocumentValidationSettings>();
}  // namespace mongo
