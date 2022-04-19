/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <boost/optional.hpp>

#include "mongo/util/assert_util_core.h"
#include "mongo/util/tick_source.h"

namespace mongo {

class SemaphoreTicketHolder;
class FifoTicketHolder;

/**
 * Move-only token that gets generated when a ticket is acquired and has to be marked invalid when
 * it is released. Only TicketHolders can create and release valid Tickets.
 */
class Ticket {
    friend class SemaphoreTicketHolder;
    friend class FifoTicketHolder;

public:
    Ticket(Ticket&& t) : _valid(t._valid) {
        t._valid = false;
    }

    Ticket& operator=(Ticket&& t) {
        invariant(!_valid);
        _valid = t._valid;
        t._valid = false;
        return *this;
    };

    ~Ticket() {
        // A Ticket can't be destroyed unless it's been released.
        invariant(!_valid);
    }

    /**
     * Returns whether or not a ticket is held. The default constructor creates an invalid Ticket
     * and the move operators mark the source object as invalid.
     */
    bool valid() {
        return _valid;
    }

private:
    Ticket() : _valid(true) {}

    void release() {
        invariant(_valid);
        _valid = false;
    }

    // No copy constructors.
    Ticket(const Ticket&) = delete;
    Ticket& operator=(const Ticket&) = delete;

    // Whether this represents a ticket being held.
    bool _valid;
};

}  // namespace mongo
