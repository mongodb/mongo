// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {
class BSONObjBuilder;

static const char kUpsertedFieldName[] = "upserted";

class NotPrimaryErrorTracker {
public:
    static const Client::Decoration<NotPrimaryErrorTracker> get;

    /**
     * Resets the object to a newly constructed state.  If "valid" is true, marks the last-error
     * object as "valid".
     */
    void reset(bool valid = false);

    /**
     * when db receives a message/request, call this
     */
    void startRequest();

    /**
     * Disables error recording for the current operation.
     */
    void disable();

    /**
     * Records the error if the error can be categorized as "NotPrimaryError".
     */
    void recordError(int code);

    bool isValid() const {
        return _valid;
    }

    bool hadError() const {
        return _hadError;
    }

    class Disabled {
    public:
        explicit Disabled(NotPrimaryErrorTracker* tracker)
            : _tracker(tracker), _prev(tracker->_disabled) {
            _tracker->_disabled = true;
        }

        ~Disabled() {
            _tracker->_disabled = _prev;
        }

    private:
        NotPrimaryErrorTracker* const _tracker;
        const bool _prev;
    };

private:
    bool _valid = false;
    bool _disabled = false;
    bool _hadError = false;
};

}  // namespace mongo
