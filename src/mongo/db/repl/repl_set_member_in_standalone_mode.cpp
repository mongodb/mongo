// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/repl_set_member_in_standalone_mode.h"

#include "mongo/db/service_context.h"
#include "mongo/util/decorable.h"

#include <utility>

namespace mongo {
namespace {

const auto& replSetMemberInStandaloneMode = ServiceContext::declareDecoration<bool>();

}  // namespace

bool getReplSetMemberInStandaloneMode(ServiceContext* serviceCtx) {
    return replSetMemberInStandaloneMode(serviceCtx);
}

void setReplSetMemberInStandaloneMode(ServiceContext* serviceCtx,
                                      bool isReplSetMemberInStandaloneMode) {
    auto& replSetMemberInStandaloneModeBool = replSetMemberInStandaloneMode(serviceCtx);
    invariant(
        !replSetMemberInStandaloneModeBool || isReplSetMemberInStandaloneMode,
        str::stream() << "replSetMemberInStandaloneModeBool: " << replSetMemberInStandaloneModeBool
                      << ", isReplSetMemberInStandaloneMode: " << isReplSetMemberInStandaloneMode);
    replSetMemberInStandaloneModeBool = isReplSetMemberInStandaloneMode;
}

}  // namespace mongo
