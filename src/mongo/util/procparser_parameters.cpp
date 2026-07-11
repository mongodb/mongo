// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/procparser_parameters.h"

#include "mongo/util/procparser.h"

namespace mongo::procparser {

Status onUpdateProcFileSizeLimit(const long long& limit) {
    setProcFileSizeLimit(limit);
    return Status::OK();
}

}  // namespace mongo::procparser
