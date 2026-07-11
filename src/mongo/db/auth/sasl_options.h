// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <atomic>
#include <string>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {

namespace optionenvironment {
class OptionSection;
class Environment;
}  // namespace optionenvironment

namespace moe = optionenvironment;

struct SASLGlobalParams;
extern SASLGlobalParams saslGlobalParams;
struct SASLGlobalParams {
    static const std::vector<std::string> kDefaultAuthenticationMechanisms;

    std::vector<std::string> authenticationMechanisms;
    std::string hostName;
    std::string serviceName;
    std::string authdPath;
    Atomic<int> scramSHA1IterationCount;
    Atomic<int> scramSHA256IterationCount;
    Atomic<int> authFailedDelay;

    SASLGlobalParams();

    static Status onSetAuthenticationMechanism(const std::vector<std::string>&) {
        saslGlobalParams.numTimesAuthenticationMechanismsSet.fetchAndAdd(1);
        return Status::OK();
    }

    static Status onSetHostName(const std::string&) {
        saslGlobalParams.haveHostName.store(true);
        return Status::OK();
    }
    static Status onSetServiceName(const std::string&) {
        saslGlobalParams.haveServiceName.store(true);
        return Status::OK();
    }
    static Status onSetAuthdPath(const std::string&) {
        saslGlobalParams.haveAuthdPath.store(true);
        return Status::OK();
    }
    static Status onSetScramSHA1IterationCount(const int) {
        saslGlobalParams.numTimesScramSHA1IterationCountSet.fetchAndAdd(1);
        return Status::OK();
    }
    static Status onSetScramSHA256IterationCount(const int) {
        saslGlobalParams.numTimesScramSHA256IterationCountSet.fetchAndAdd(1);
        return Status::OK();
    }


    Atomic<int> numTimesAuthenticationMechanismsSet;
    Atomic<bool> haveHostName;
    Atomic<bool> haveServiceName;
    Atomic<bool> haveAuthdPath;
    Atomic<int> numTimesScramSHA1IterationCountSet;
    Atomic<int> numTimesScramSHA256IterationCountSet;
};

[[MONGO_MOD_PRIVATE]] Status addSASLOptions(moe::OptionSection* options);

[[MONGO_MOD_PRIVATE]] Status storeSASLOptions(const moe::Environment& params);

}  // namespace mongo
