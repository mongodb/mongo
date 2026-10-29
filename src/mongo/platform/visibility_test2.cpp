// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/visibility_test_lib2.h"
#include "mongo/util/exit_code.h"

#include <memory>

int main(int argc, char* argv[]) {
    mongo::visibility_test_lib2::Derived d("hello", argc);
    return (d.value() == argc) ? static_cast<int>(mongo::ExitCode::clean)
                               : static_cast<int>(mongo::ExitCode::fail);
}
