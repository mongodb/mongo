// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/not_primary_error_tracker.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <utility>

namespace mongo {

const Client::Decoration<NotPrimaryErrorTracker> NotPrimaryErrorTracker::get =
    Client::declareDecoration<NotPrimaryErrorTracker>();

void NotPrimaryErrorTracker::reset(bool valid) {
    *this = NotPrimaryErrorTracker();
    _valid = valid;
}

void NotPrimaryErrorTracker::recordError(int code) {
    if (_disabled) {
        return;
    }
    reset(true);
    if (ErrorCodes::isNotPrimaryError(ErrorCodes::Error(code)))
        _hadError = true;
}

void NotPrimaryErrorTracker::disable() {
    invariant(!_disabled);
    _disabled = true;
}

void NotPrimaryErrorTracker::startRequest() {
    _disabled = false;
    _hadError = false;
}

}  // namespace mongo
