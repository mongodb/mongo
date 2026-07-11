// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
namespace mongo {

/**
 * Asserts that the given MatchExpression 'match' serializes to the BSONObj 'expected'.
 */
#define ASSERT_SERIALIZES_TO(match, expected)            \
    do {                                                 \
        ASSERT_BSONOBJ_EQ(match->serialize(), expected); \
    } while (false)

}  // namespace mongo
