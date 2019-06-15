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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/kv/kv_collection_catalog_entry.h"

#include <memory>

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/kv/kv_catalog_feature_tracker.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/kv/storage_engine_impl.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;

KVCollectionCatalogEntry::KVCollectionCatalogEntry(StorageEngineInterface* engine,
                                                   StringData ns,
                                                   StringData ident,
                                                   std::unique_ptr<RecordStore> rs)
    : BSONCollectionCatalogEntry(ns),
      _engine(engine),
      _ident(ident.toString()),
      _recordStore(std::move(rs)) {}
}
