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

#pragma once

#include "mongo/db/repl/intent_registry.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo::rss::consensus {

/**
 * RAII class used to handle registering and deregistering intents with the IntentRegistration
 * system.
 */
class MONGO_MOD_PUB IntentGuard {
public:
    /**
     * Registers an intent. If the intent is not granted, an exception will be thrown. It will be up
     * to the caller to handle the case when the intent is not granted.
     */
    IntentGuard(IntentRegistry::Intent intent, OperationContext* opctx);

    IntentGuard(const IntentGuard&) = delete;
    IntentGuard& operator=(const IntentGuard&) = delete;

    IntentGuard(IntentGuard&& o) noexcept : _opCtx(std::exchange(o._opCtx, {})), _token(o._token) {}

    IntentGuard& operator=(IntentGuard&& o) noexcept {
        if (this != &o) {
            _opCtx = std::exchange(o._opCtx, {});
            _token = o._token;
        }
        return *this;
    }

    /**
     * Deregisters the intent.
     */
    ~IntentGuard() {
        reset();
    }

    OperationContext* getOperationContext() const {
        return _opCtx;
    }

    /**
     * Returns the type of intent that was granted, or boost::none if no intent was granted.
     */
    boost::optional<IntentRegistry::Intent> intent() const;

    /**
     * Deregisters the intent but without destructing the IntentGuard object.
     */
    void reset();

private:
    OperationContext* _opCtx;
    IntentRegistry::IntentToken _token;
};

/**
 * A wrapper class around IntentGuard that is only used for Write Intents.
 */
class MONGO_MOD_PUB WriteIntentGuard : public IntentGuard {
public:
    explicit WriteIntentGuard(OperationContext* opCtx)
        : IntentGuard(IntentRegistry::Intent::Write, opCtx) {}
};
}  // namespace mongo::rss::consensus
