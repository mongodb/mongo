// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/visibility_test_lib1.h"
#include "mongo/util/exit_code.h"

#include <memory>
#include <string_view>

int main(int argc, char* argv[]) {
    mongo::visibility_test_lib1::Base b("hello");
    return (b.name() == "hello") ? static_cast<int>(mongo::ExitCode::clean)
                                 : static_cast<int>(mongo::ExitCode::fail);
}
