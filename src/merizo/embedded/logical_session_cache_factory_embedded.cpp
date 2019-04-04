/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MERIZO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kControl

#include "merizo/platform/basic.h"

#include <memory>

#include "merizo/embedded/logical_session_cache_factory_embedded.h"

#include "merizo/db/logical_session_cache_impl.h"
#include "merizo/db/service_liaison_merizod.h"
#include "merizo/db/sessions_collection_standalone.h"
#include "merizo/stdx/memory.h"
#include "merizo/util/log.h"

namespace merizo {

std::unique_ptr<LogicalSessionCache> makeLogicalSessionCacheEmbedded() {
    auto liaison = std::make_unique<ServiceLiaisonMerizod>();

    // Set up the logical session cache
    auto sessionsColl = std::make_shared<SessionsCollectionStandalone>();

    return stdx::make_unique<LogicalSessionCacheImpl>(
        std::move(liaison), std::move(sessionsColl), nullptr, LogicalSessionCacheImpl::Options{});
}

}  // namespace merizo
