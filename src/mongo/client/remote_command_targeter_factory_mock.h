// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/remote_command_targeter_factory.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/util/modules.h"

#include <map>
#include <memory>

namespace mongo {

/**
 * Factory which instantiates mock remote command targeters. This class is not thread-safe and is
 * only used for unit-testing.
 */
class [[MONGO_MOD_PUBLIC]] RemoteCommandTargeterFactoryMock final
    : public RemoteCommandTargeterFactory {
public:
    RemoteCommandTargeterFactoryMock();
    ~RemoteCommandTargeterFactoryMock() override;

    /**
     * If the input connection string matches one of the pre-defined targeters added through an
     * earlier call to addTargetersToReturn, pops one of these targeters from the map and returns
     * it. Otherwise, creates a new instance of RemoteCommandTargeterMock.
     */
    std::unique_ptr<RemoteCommandTargeter> create(const ConnectionString& connStr) override;

    /**
     * Specifies a targeter entry, proxy to which should be returned every time the specified
     * connection string is used.
     */
    void addTargeterToReturn(const ConnectionString& connStr,
                             std::unique_ptr<RemoteCommandTargeterMock> mockTargeter);

    /**
     * Removes a targeter previous installed through a call to addTargeterToReturn. It is illegal
     * to call this method if there is no registered targeter for the specified connection string
     * or of there are any outstanding targeter proxies (i.e. targeters returned by the create
     * call, which have not been released).
     */
    void removeTargeterToReturn(const ConnectionString& connStr);

private:
    using MockTargetersMap = std::map<ConnectionString, std::shared_ptr<RemoteCommandTargeterMock>>;

    // Map of pre-defined targeters, proxies to which should be returned as part of the create call
    MockTargetersMap _mockTargeters;
};

}  // namespace mongo
