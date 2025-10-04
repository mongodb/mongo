/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/local_catalog/health_log.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/health_log_gen.h"
#include "mongo/db/namespace_string.h"

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
