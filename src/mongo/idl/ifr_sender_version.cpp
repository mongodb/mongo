// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/idl/ifr_sender_version.h"

#include "mongo/util/str.h"

namespace mongo {

std::string formatSenderVersion(const IFRSenderVersion& v) {
    str::stream s;
    s << v.getMajor() << "." << v.getMinor() << "." << v.getPatch();
    // 'extra' is IDL-constrained to <= 0, so a non-zero value is negative and carries its own sign.
    if (v.getExtra() != 0) {
        s << v.getExtra();
    }
    return s;
}

IFRSenderVersion makeIFRSenderVersion(const VersionInfoInterface& provider) {
    IFRSenderVersion version;
    version.setMajor(provider.majorVersion());
    version.setMinor(provider.minorVersion());
    version.setPatch(provider.patchVersion());
    version.setExtra(provider.extraVersion());
    return version;
}

IFRSenderVersion makeLocalIFRSenderVersion() {
    return makeIFRSenderVersion(VersionInfoInterface::instance());
}

}  // namespace mongo
