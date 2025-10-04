/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/auth/sasl_mechanism_policies.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"

namespace mongo::auth {

class SaslX509ServerMechanism final : public MakeServerMechanism<X509Policy> {
public:
    explicit SaslX509ServerMechanism(std::string authenticationDatabase)
        : MakeServerMechanism<X509Policy>(std::move(authenticationDatabase)) {}

    ~SaslX509ServerMechanism() override = default;

    StatusWith<std::tuple<bool, std::string>> stepImpl(OperationContext* opCtx,
                                                       StringData inputData) override;

    boost::optional<unsigned int> currentStep() const override {
        return _step;
    }

    boost::optional<unsigned int> totalSteps() const override {
        return kMaxStep;
    }

    bool isClusterMember(Client* client) const override;

    StatusWith<std::unique_ptr<UserRequest>> makeUserRequest(
        OperationContext* opCtx) const override;

private:
    static constexpr unsigned int kMaxStep = 1;

    unsigned int _step{0};
};

class X509ServerFactory : public MakeServerFactory<SaslX509ServerMechanism> {
public:
    using MakeServerFactory<SaslX509ServerMechanism>::MakeServerFactory;
    static constexpr bool isInternal = false;
    bool canMakeMechanismForUser(const User* user) const final {
        auto credentials = user->getCredentials();
        return credentials.isExternal;
    }
};

}  // namespace mongo::auth
