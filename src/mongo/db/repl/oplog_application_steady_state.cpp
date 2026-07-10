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

#include "mongo/db/repl/oplog_application_steady_state.h"

namespace mongo::repl {

// ServerParameter has no built-in mechanism to distinguish "value equals the default because the
// user never set it" from "value equals the default because the user explicitly set it to the
// default value." The only available hook is on_update, which fires whenever the parameter is set
// by the user (via --setParameter at startup or the setParameter command at runtime) but never
// fires when the IDL default is used. This is the same pattern used for oplogMinRetentionHours and
// oplog size — the difference is that those are direct CLI flags whose "initialized using default"
// tracking is set in mongod_options.cpp, while server parameters require on_update to achieve the
// same effect.
//
// ServerParameterWithStorage::setDefault is not an alternative: it internally calls reset(), which
// overwrites the current value with the new default. The persistence provider is only known at
// ServiceContext::make() time (via ConstructorActionRegisterer), which runs after
// runGlobalInitializers() — meaning --setParameter has already been applied. Calling setDefault at
// that point would silently clobber any explicit operator configuration.
Atomic<bool> oplogApplicationEnforcesSteadyStateConstraintsInitializedUsingDefault{true};

Status onOplogApplicationEnforcesSteadyStateConstraintsUpdate(bool) {
    oplogApplicationEnforcesSteadyStateConstraintsInitializedUsingDefault.store(false);
    return Status::OK();
}

}  // namespace mongo::repl
