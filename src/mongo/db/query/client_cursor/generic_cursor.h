// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/query/client_cursor/generic_cursor_gen.h"
#include "mongo/util/modules.h"

#include <ostream>

namespace mongo {

inline std::ostream& operator<<(std::ostream& s, const GenericCursor& gc) {
    return (s << gc.toBSON());
}

inline StringBuilder& operator<<(StringBuilder& s, const GenericCursor& gc) {
    return (s << gc.toBSON());
}

}  // namespace mongo
