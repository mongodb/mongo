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

#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <boost/optional.hpp>

MONGO_MOD_PUBLIC;
namespace mongo::index_builds::primary_driven {

/**
 * State that coordinates redoing a write which produced a "tearable side write" during a
 * primary-driven index build. When a concurrent collection write produces a batched write that
 * splits into more than one applyOps entry (a chain that may "tear" across applyOps boundaries,
 * making the build unsafe to resume), a write conflict is thrown  to redo the write. On the redo,
 * an abort sentinel is written into the affected build's resumable-state container before the side
 * write, so it replicates first; on resume the sentinel fails to parse and the build is aborted.
 *
 * TODO (SERVER-126257): Remove once index build side writes cannot be torn.
 */
class TearableSideWriteRedoState {
public:
    void arm(const UUID& collectionUUID) {
        _redoCollectionUUID = collectionUUID;
    }
    void disarm() {
        _redoCollectionUUID = boost::none;
    }
    boost::optional<UUID> armedCollectionUUID() const {
        return _redoCollectionUUID;
    }

    void setFlagPersisted() {
        _flagPersisted = true;
    }
    void resetFlagPersisted() {
        _flagPersisted = false;
    }
    bool flagPersisted() const {
        return _flagPersisted;
    }

private:
    boost::optional<UUID> _redoCollectionUUID;
    bool _flagPersisted = false;
};

/**
 * Returns the TearableSideWriteRedoState decorating the given OperationContext.
 */
TearableSideWriteRedoState& getTearableSideWriteRedoState(OperationContext* opCtx);

}  // namespace mongo::index_builds::primary_driven
