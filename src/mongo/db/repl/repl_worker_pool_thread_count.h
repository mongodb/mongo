// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/base/status.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace [[MONGO_MOD_PARENT_PRIVATE]] repl {

size_t getMinThreadCountForReplWorkerPool();
size_t getThreadCountForReplWorkerPool();
Status validateUpdateReplWriterThreadCount(int count, const boost::optional<TenantId>&);
Status onUpdateReplWriterThreadCount(int);
Status validateUpdateReplWriterMinThreadCount(int count, const boost::optional<TenantId>&);
Status onUpdateReplWriterMinThreadCount(int);

}  // namespace repl
}  // namespace mongo
