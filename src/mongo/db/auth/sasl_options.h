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

#pragma once

#include "mongo/base/status.h"
#include "mongo/platform/atomic_word.h"

#include <atomic>
#include <string>
#include <vector>

namespace mongo {

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

Status addSASLOptions(moe::OptionSection* options);

Status storeSASLOptions(const moe::Environment& params);

}  // namespace mongo
