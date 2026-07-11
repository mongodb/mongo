// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <boost/optional.hpp>

[[MONGO_MOD_PUBLIC]];
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
