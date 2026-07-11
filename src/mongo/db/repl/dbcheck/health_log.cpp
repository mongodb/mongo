// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/dbcheck/health_log.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/dbcheck/health_log_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"

#include <cstdint>

namespace mongo {

namespace {
const int64_t kDefaultHealthlogSize = 100'000'000;

CollectionOptions getOptions(void) {
    CollectionOptions options;
    options.capped = true;
    options.cappedSize = kDefaultHealthlogSize;
    options.setNoIdIndex();
    return options;
}
}  // namespace

HealthLog::HealthLog()
    : _writer(NamespaceString::kLocalHealthLogNamespace,
              getOptions(),
              kMaxBufferSize,
              // Writing to the 'local' database is permitted on all nodes, not just the primary.
              true /*retryOnReplStateChangeInterruption*/) {}

void HealthLog::startup() {
    _writer.startup(std::string("healthlog writer"));
}

void HealthLog::shutdown() {
    _writer.shutdown();
}

bool HealthLog::log(const HealthLogEntry& entry) {
    BSONObjBuilder builder;
    OID oid;
    oid.init();
    builder.append("_id", oid);
    entry.serialize(&builder);
    return _writer.insertDocument(builder.obj());
}
}  // namespace mongo
