// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <iosfwd>

namespace mongo::logv2 {

/**
 * Representation of the console.  Use this in place of cout/cin, in applications that write to
 * the console from multiple threads (such as those that use the logging subsystem).
 *
 * The Console type is synchronized such that only one instance may be in the fully constructed
 * state at a time.  Correct usage is to instantiate one, write or read from it as desired, and
 * then destroy it.
 *
 * The console streams accept UTF-8 encoded data, and attempt to write it to the attached
 * console faithfully.
 *
 * TODO(schwerin): If no console is attached on Windows (services), should writes here go to the
 * event logger?
 */
class [[MONGO_MOD_PUBLIC]] Console {
public:
    Console();

    static std::ostream& out();
};

}  // namespace mongo::logv2
