// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/idl/idl_test_defs.h"

#include "mongo/idl/unittest_gen.h"
#include "mongo/idl/unittest_import_gen.h"

namespace mongo::idl::test {

void checkValuesEqual(StructWithValidator* s) {
    uassert(6253512, "Values not equal", s->getFirst() == s->getSecond());
}

}  // namespace mongo::idl::test
