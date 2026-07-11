// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/matcher/match_details.h"

#include "mongo/util/assert_util.h"

#include <sstream>

namespace mongo {

using std::string;

MatchDetails::MatchDetails() : _elemMatchKeyRequested() {
    resetOutput();
}

void MatchDetails::resetOutput() {
    _loadedRecord = false;
    _elemMatchKey.reset();
}

bool MatchDetails::hasElemMatchKey() const {
    return _elemMatchKey.get();
}

std::string MatchDetails::elemMatchKey() const {
    MONGO_verify(hasElemMatchKey());
    return *(_elemMatchKey.get());
}

void MatchDetails::setElemMatchKey(const std::string& elemMatchKey) {
    if (_elemMatchKeyRequested) {
        _elemMatchKey.reset(new std::string(elemMatchKey));
    }
}

string MatchDetails::toString() const {
    std::stringstream ss;
    ss << "loadedRecord: " << _loadedRecord << " ";
    ss << "elemMatchKeyRequested: " << _elemMatchKeyRequested << " ";
    ss << "elemMatchKey: " << (_elemMatchKey ? _elemMatchKey->c_str() : "NONE") << " ";
    return ss.str();
}
}  // namespace mongo
