// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/time_support.h"

namespace mongo {

class [[MONGO_MOD_PUBLIC]] CertificateExpirationMonitor {
public:
    /**
     * Get the singleton instance of the monitor.
     */
    static CertificateExpirationMonitor* get();

    /**
     * Sets the server certificate's expiration deadline.
     */
    void updateExpirationDeadline(Date_t date);

    /**
     * Kick off the CertificateExpirationMonitor background job.
     */
    void start(ServiceContext* service);

private:
    void run(Client* client);

    std::unique_ptr<PeriodicJobAnchor> _job;

    std::mutex _mutex;
    Date_t _certExpiration{Date_t::max()};
};

}  // namespace mongo
