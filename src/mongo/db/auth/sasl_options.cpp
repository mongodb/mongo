// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/sasl_options.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/db/auth/auth_mechanism.h"
#include "mongo/db/auth/sasl_options_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/text.h"  // IWYU pragma: keep

namespace mongo {

const std::vector<std::string> SASLGlobalParams::kDefaultAuthenticationMechanisms =
    std::vector<std::string>{std::string{auth::kMechanismMongoX509},
                             std::string{auth::kMechanismScramSha1},
                             std::string{auth::kMechanismScramSha256}};
SASLGlobalParams saslGlobalParams;

SASLGlobalParams::SASLGlobalParams() {
    scramSHA1IterationCount.store(kScramIterationCountDefault);
    scramSHA256IterationCount.store(kScramSHA256IterationCountDefault);
    numTimesAuthenticationMechanismsSet.store(0);
    haveHostName.store(false);
    haveServiceName.store(false);
    haveAuthdPath.store(false);
    numTimesScramSHA1IterationCountSet.store(0);
    numTimesScramSHA256IterationCountSet.store(0);
    authenticationMechanisms = kDefaultAuthenticationMechanisms;

    // Default value for auth failed delay
    authFailedDelay.store(0);
}

namespace {
MONGO_INITIALIZER_WITH_PREREQUISITES(InitSpeculativeCounters, ("EndStartupOptionStorage"))
(InitializerContext*) {
    authCounter.initializeMechanismMap(saslGlobalParams.authenticationMechanisms);
}
}  // namespace

}  // namespace mongo
