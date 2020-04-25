/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include "mongo/util/net/private/ssl_expiration.h"

#include <string>

#include "mongo/logv2/log.h"
#include "mongo/util/time_support.h"

namespace mongo {

static const auto oneDay = Hours(24);

CertificateExpirationMonitor::CertificateExpirationMonitor(Date_t date)
    : _certExpiration(date), _lastCheckTime(Date_t::now()) {}

std::string CertificateExpirationMonitor::taskName() const {
    return "CertificateExpirationMonitor";
}

void CertificateExpirationMonitor::taskDoWork() {
    const Milliseconds timeSinceLastCheck = Date_t::now() - _lastCheckTime;

    if (timeSinceLastCheck < oneDay)
        return;

    const Date_t now = Date_t::now();
    _lastCheckTime = now;

    if (_certExpiration <= now) {
        // The certificate has expired.
        LOGV2_WARNING(23785,
                      "Server certificate is now invalid. It expired on {certExpiration}",
                      "Server certificate has expired",
                      "certExpiration"_attr = dateToISOStringUTC(_certExpiration));
        return;
    }

    const auto remainingValidDuration = _certExpiration - now;

    if (remainingValidDuration <= 30 * oneDay) {
        // The certificate will expire in the next 30 days.
        LOGV2_WARNING(23786,
                      "Server certificate will expire on {certExpiration} in "
                      "{validDuration}.",
                      "Server certificate will expire soon",
                      "certExpiration"_attr = dateToISOStringUTC(_certExpiration),
                      "validDuration"_attr = durationCount<Hours>(remainingValidDuration));
    }
}

}  // namespace mongo
