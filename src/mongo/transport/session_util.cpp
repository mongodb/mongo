/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/transport/session_util.h"

namespace mongo::transport::util {
bool isExemptedByCIDRList(const SockAddr& ra,
                          const SockAddr& la,
                          const std::vector<std::variant<CIDR, std::string>>& exemptions) {
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
