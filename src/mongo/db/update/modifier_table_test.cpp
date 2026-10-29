// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update/modifier_table.h"

#include "mongo/unittest/unittest.h"


namespace {

using namespace mongo::modifiertable;


TEST(getType, Normal) {
    ASSERT_EQUALS(getType("$set"), MOD_SET);
    ASSERT_EQUALS(getType("$AModThatDoesn'tExist"), MOD_UNKNOWN);
    ASSERT_EQUALS(getType("NotAModExpression"), MOD_UNKNOWN);
}

}  // unnamed namespace
