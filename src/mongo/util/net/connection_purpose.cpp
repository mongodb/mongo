// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/util/net/connection_purpose.h"

namespace mongo {
namespace {
const auto getForSession = transport::Session::declareDecoration<ConnectionPurpose>();
}  // namespace
ConnectionPurpose& getConnectionPurpose(transport::Session* session) {
    return getForSession(session);
}
}  // namespace mongo
