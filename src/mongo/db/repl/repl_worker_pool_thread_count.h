// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"


namespace mongo {
namespace [[MONGO_MOD_PARENT_PRIVATE]] repl {

size_t getMinThreadCountForReplWorkerPool();
size_t getThreadCountForReplWorkerPool();
Status validateUpdateReplWriterThreadCount(int count);
Status onUpdateReplWriterThreadCount(int);
Status validateUpdateReplWriterMinThreadCount(int count);
Status onUpdateReplWriterMinThreadCount(int);

}  // namespace repl
}  // namespace mongo
