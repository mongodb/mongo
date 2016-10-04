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

#include <map>

#include "mongo/client/remote_command_targeter_factory.h"

namespace mongo {

class RemoteCommandTargeterMock;

/**
 * Factory which instantiates mock remote command targeters. This class is not thread-safe and is
 * only used for unit-testing.
 */
class RemoteCommandTargeterFactoryMock final : public RemoteCommandTargeterFactory {
public:
    RemoteCommandTargeterFactoryMock();
    ~RemoteCommandTargeterFactoryMock();

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
