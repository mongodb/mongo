/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/db/version_context.h"

namespace mongo {

static auto getVersionContext = OperationContext::declareDecoration<VersionContext>();

const VersionContext& VersionContext::getDecoration(const OperationContext* opCtx) {
    return getVersionContext(opCtx);
}

void VersionContext::setDecoration(ClientLock&,
                                   OperationContext* opCtx,
                                   const VersionContext& vCtx) {
    tassert(9955801, "Expected incoming versionContext to be initialized", vCtx.isInitialized());

    // We disallow setting a VersionContext decoration multiple times on the same OperationContext,
    // even if it's the same one. There is no use case for it, and makes our implementation more
    // complex (e.g. ScopedSetDecoration would need to have a "no-op" destructor path).
    auto& originalVCtx = getVersionContext(opCtx);
    tassert(10296500,
            "Refusing to set a VersionContext on an operation that already has one",
            !originalVCtx.isInitialized());
    originalVCtx = vCtx;
}

void VersionContext::setFromMetadata(ClientLock& lk,
                                     OperationContext* opCtx,
                                     const VersionContext& vCtx) {
    VersionContext::setDecoration(lk, opCtx, vCtx);
}

VersionContext::ScopedSetDecoration::ScopedSetDecoration(OperationContext* opCtx,
                                                         const VersionContext& vCtx)
    : _opCtx(opCtx) {
    ClientLock lk(opCtx->getClient());
    VersionContext::setDecoration(lk, opCtx, vCtx);
}

VersionContext::ScopedSetDecoration::~ScopedSetDecoration() {
    ClientLock lk(_opCtx->getClient());
    getVersionContext(_opCtx).resetToOperationWithoutOFCV();
}

}  // namespace mongo
