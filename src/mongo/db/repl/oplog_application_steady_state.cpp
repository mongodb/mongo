// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
