// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/idl/ifr_sender_version.h"
#include "mongo/util/modules.h"

#include <map>
#include <memory>
#include <string>

namespace mongo {

/**
 * Extra information attached to an UnrecognizedIFRFlag error. Carries all flags that the receiver
 * did not recognize and the version of the sender, allowing upstream handlers to log or propagate
 * the full set without re-parsing the reason string.
 */
class [[MONGO_MOD_PUBLIC]] UnrecognizedIFRFlagInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::UnrecognizedIFRFlag;

    // Flags are keyed by name so repeated names in a single payload collapse to one entry, and
    // iteration order (used for serialization) is stable.
    using FlagMap = std::map<std::string, bool>;

    UnrecognizedIFRFlagInfo(FlagMap flags, IFRSenderVersion senderVersion)
        : _flags(std::move(flags)), _senderVersion(std::move(senderVersion)) {}

    const FlagMap& getFlags() const {
        return _flags;
    }

    const IFRSenderVersion& getSenderVersion() const {
        return _senderVersion;
    }

    void serialize(BSONObjBuilder* bob) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);

private:
    FlagMap _flags;
    IFRSenderVersion _senderVersion;
};

/**
 * Builds the Status reported when a same-or-older sender sends unrecognized IFR flags: an
 * `UnrecognizedIFRFlag` status carrying `UnrecognizedIFRFlagInfo` and a human-readable reason
 * (singular vs. plural wording depending on how many flags were unrecognized). Factored out of the
 * throw site so the error contents can be asserted directly in tests without triggering the
 * deferred tripwire.
 */
[[MONGO_MOD_PUBLIC]] Status makeUnrecognizedIFRFlagStatus(
    UnrecognizedIFRFlagInfo::FlagMap unknownFlags, IFRSenderVersion senderVersion);

}  // namespace mongo
