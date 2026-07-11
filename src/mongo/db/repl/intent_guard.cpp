// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/intent_guard.h"

namespace mongo::rss::consensus {
IntentGuard::IntentGuard(IntentRegistry::Intent intent, OperationContext* opctx)
    : _opCtx(opctx),
      _svcCtx(_opCtx->getClient()->getServiceContext()),
      _token(IntentRegistry::get(_svcCtx).registerIntent(intent, _opCtx)) {}

void IntentGuard::reset() {
    if (_svcCtx) {
        IntentRegistry::get(_svcCtx).deregisterIntent(_token);
        _svcCtx = nullptr;
        _opCtx = nullptr;
    }
}

boost::optional<IntentRegistry::Intent> IntentGuard::intent() const {
    if (!_opCtx) {
        return boost::none;
    }
    return _token.intent();
}

}  // namespace mongo::rss::consensus
