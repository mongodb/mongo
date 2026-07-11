// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/idl/ifr_sender_version.h"

namespace mongo {

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
