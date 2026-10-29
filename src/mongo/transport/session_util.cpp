// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/session_util.h"

namespace mongo::transport::util {
bool isExemptedByCIDRList(const SockAddr& ra, const SockAddr& la, const CIDRList& exemptions) {
    if (exemptions.empty())
        return false;

    boost::optional<CIDR> remoteCIDR;
    if (ra.isValid() && ra.isIP())
        remoteCIDR = uassertStatusOK(CIDR::parse(ra.getAddr()));

    return std::any_of(exemptions.begin(), exemptions.end(), [&](const auto& exemption) {
        return visit(
            [&](auto&& ex) {
                using Alt = std::decay_t<decltype(ex)>;
                if constexpr (std::is_same_v<Alt, CIDR>)
                    return remoteCIDR && ex.contains(*remoteCIDR);
#ifndef _WIN32
                // Otherwise the exemption is a UNIX path and we should check the local path
                // (the remoteAddr == "anonymous unix socket") against the exemption string.
                // On Windows we don't check this at all and only CIDR ranges are supported.
                if constexpr (std::is_same_v<Alt, std::string>)
                    return la.isValid() && la.getAddr() == ex;
#endif
                return false;
            },
            exemption);
    });
}
}  // namespace mongo::transport::util
