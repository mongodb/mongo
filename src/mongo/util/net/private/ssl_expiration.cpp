// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/net/private/ssl_expiration.h"

#include "mongo/logv2/log.h"
#include "mongo/util/time_support.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

static const auto oneDay = Hours(24);

CertificateExpirationMonitor* theCertificateExpirationMonitor;

void CertificateExpirationMonitor::updateExpirationDeadline(Date_t date) {
    std::lock_guard<std::mutex> lock(_mutex);
    _certExpiration = date;
}

CertificateExpirationMonitor* CertificateExpirationMonitor::get() {
    if (!theCertificateExpirationMonitor) {
        theCertificateExpirationMonitor = new CertificateExpirationMonitor();
    }
    return theCertificateExpirationMonitor;
}

void CertificateExpirationMonitor::start(ServiceContext* service) {
    std::lock_guard<std::mutex> lock(_mutex);

    auto periodicRunner = service->getPeriodicRunner();
    invariant(periodicRunner);

    // The certificate expiration monitor is technically killable, but since it never creates an
    // operation context, it will never actually be interrupted.
    PeriodicRunner::PeriodicJob job(
        "CertificateExpirationMonitor",
        [this](Client* client) { return run(client); },
        oneDay,
        true /*isKillableByStepdown*/);

    _job = std::make_unique<PeriodicJobAnchor>(periodicRunner->makeJob(std::move(job)));
    _job->start();
}

void CertificateExpirationMonitor::run(Client*) {
    const Date_t now = Date_t::now();
    std::lock_guard<std::mutex> lock(_mutex);
    if (_certExpiration <= now) {
        // The certificate has expired.
        LOGV2_WARNING(23785,
                      "Server certificate has expired",
                      "certExpiration"_attr = dateToISOStringUTC(_certExpiration));
        return;
    }

    const auto remainingValidDuration = _certExpiration - now;

    if (remainingValidDuration <= 30 * oneDay) {
        // The certificate will expire in the next 30 days
        LOGV2_WARNING(23786,
                      "Server certificate will expire soon",
                      "certExpiration"_attr = dateToISOStringUTC(_certExpiration),
                      "validDuration"_attr = durationCount<Hours>(remainingValidDuration));
    }
}

}  // namespace mongo
