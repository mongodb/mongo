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

#include "mongo/db/auth/sasl_options.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/db/auth/sasl_options_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/text.h"  // IWYU pragma: keep

namespace mongo {

const std::vector<std::string> SASLGlobalParams::kDefaultAuthenticationMechanisms =
    std::vector<std::string>{"MONGODB-X509", "SCRAM-SHA-1", "SCRAM-SHA-256"};
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
