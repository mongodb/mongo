// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/visitors/docs_needed_bounds_gen.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

/*
 * Includes utility functions related to DocsNeededBounds.
 */

namespace mongo::docs_needed_bounds {
/*
 * For queries where the upper and lower limit of docs needed is known as a discrete long long
 * value, the extractable limit can be computed.
 */
boost::optional<long long> calcExtractableLimit(DocsNeededBounds docsNeededBounds);
};  // namespace mongo::docs_needed_bounds
