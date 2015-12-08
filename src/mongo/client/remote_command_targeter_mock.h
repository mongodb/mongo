/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class RemoteCommandTargeterMock final : public RemoteCommandTargeter {
public:
    RemoteCommandTargeterMock();
    virtual ~RemoteCommandTargeterMock();

    /**
     * Shortcut for unit-tests.
     */
    static std::shared_ptr<RemoteCommandTargeterMock> get(
        std::shared_ptr<RemoteCommandTargeter> targeter);

    /**
     * Returns the value last set by setConnectionStringReturnValue.
     */
    ConnectionString connectionString() override;

    /**
     * Returns the return value last set by setFindHostReturnValue.
     * Returns ErrorCodes::InternalError if setFindHostReturnValue was never called.
     */
    StatusWith<HostAndPort> findHost(const ReadPreferenceSetting& readPref,
                                     Milliseconds maxWait) override;

    /**
     * No-op for the mock.
     */
    void markHostNotMaster(const HostAndPort& host) override;

    /**
     * No-op for the mock.
     */
    void markHostUnreachable(const HostAndPort& host) override;

    /**
     * Sets the return value for the next call to connectionString.
     */
    void setConnectionStringReturnValue(const ConnectionString returnValue);

    /**
     * Sets the return value for the next call to findHost.
     */
    void setFindHostReturnValue(StatusWith<HostAndPort> returnValue);

private:
    ConnectionString _connectionStringReturnValue;
    StatusWith<HostAndPort> _findHostReturnValue;
};

}  // namespace mongo
