/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/server_options.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {
class OperationContext;
class VersionContext;
}  // namespace mongo

namespace mongo::index_builds::primary_driven {

/**
 * Returns true if the provider mandates primary-driven index builds or the feature flag is
 * enabled. Acquires its own FCV snapshot.
 */
bool enabled(OperationContext* opCtx);

/**
 * Returns true if the provider mandates primary-driven index builds or the feature flag is
 * enabled. Uses the supplied FCV snapshot and the operation's decorated version context.
 */
bool enabled(OperationContext* opCtx, const ServerGlobalParams::FCVSnapshot& fcvSnapshot);

/**
 * Returns true if the provider mandates primary-driven index builds or the feature flag is
 * enabled. Uses the supplied version context and FCV snapshot.
 */
bool enabled(OperationContext* opCtx,
             const VersionContext& vCtx,
             const ServerGlobalParams::FCVSnapshot& fcvSnapshot);

}  // namespace mongo::index_builds::primary_driven
