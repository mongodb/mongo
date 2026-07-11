// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/exit_code.h"

namespace mongo {

ExitCode mongos_main(int argc, char* argv[]);

}  // namespace mongo
