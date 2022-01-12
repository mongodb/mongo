/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/change_stream_options_manager.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {
const auto getChangeStreamOptionsManager =
    ServiceContext::declareDecoration<boost::optional<ChangeStreamOptionsManager>>();

}  // namespace

ChangeStreamOptionsManager& ChangeStreamOptionsManager::get(ServiceContext* service) {
    return *getChangeStreamOptionsManager(service);
}

ChangeStreamOptionsManager& ChangeStreamOptionsManager::get(OperationContext* opCtx) {
    return *getChangeStreamOptionsManager(opCtx->getServiceContext());
}

void ChangeStreamOptionsManager::create(ServiceContext* service) {
    getChangeStreamOptionsManager(service).emplace(service);
}

boost::optional<ChangeStreamOptions> ChangeStreamOptionsManager::getOptions(
    OperationContext* opCtx) {
    stdx::lock_guard<Latch> L(_mutex);
    return _changeStreamOptions;
}

StatusWith<ChangeStreamOptions> ChangeStreamOptionsManager::setOptions(
    OperationContext* opCtx, ChangeStreamOptions optionsToSet) {
    stdx::lock_guard<Latch> L(_mutex);
    _changeStreamOptions = std::move(optionsToSet);
    return *_changeStreamOptions;
}

}  // namespace mongo
