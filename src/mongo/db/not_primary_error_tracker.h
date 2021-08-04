/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/client.h"

namespace mongo {
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
