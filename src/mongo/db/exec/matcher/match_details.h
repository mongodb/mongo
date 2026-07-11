// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <memory>
#include <string>

namespace mongo {

/**
 * Reports information about a match request.
 *
 * TODO SERVER-113198: Remove external dependency on this class.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] MatchDetails {
public:
    MatchDetails();

    void resetOutput();

    // for debugging only
    std::string toString() const;

    // relating to whether or not we had to load the full record

    void setLoadedRecord(bool loadedRecord) {
        _loadedRecord = loadedRecord;
    }

    bool hasLoadedRecord() const {
        return _loadedRecord;
    }

    // this name is wrong

    bool needRecord() const {
        return _elemMatchKeyRequested;
    }

    // if we need to store the offset into an array where we found the match

    /** Request that an elemMatchKey be recorded. */
    void requestElemMatchKey() {
        _elemMatchKeyRequested = true;
    }

    bool hasElemMatchKey() const;
    std::string elemMatchKey() const;

    void setElemMatchKey(const std::string& elemMatchKey);

private:
    bool _loadedRecord;
    bool _elemMatchKeyRequested;
    std::unique_ptr<std::string> _elemMatchKey;
};
}  // namespace mongo
