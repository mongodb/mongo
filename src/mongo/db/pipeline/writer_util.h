// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once


#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/modules.h"

namespace mongo {

BatchedCommandRequest makeInsertCommand(const NamespaceString& outputNs,
                                        bool bypassDocumentValidation);

}  // namespace mongo
