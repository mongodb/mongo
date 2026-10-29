// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {
class DocumentSourceListSearchIndexesSpec;
/**
 * Function used by the IDL parser to validate that only one field (either name or id) is specified
 * at a time. Note that it is okay for neither field to be specified.
 */
void validateListSearchIndexesSpec(const DocumentSourceListSearchIndexesSpec* spec);

}  // namespace mongo
