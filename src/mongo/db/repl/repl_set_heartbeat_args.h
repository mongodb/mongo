/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <string>

#include "mongo/util/net/hostandport.h"

namespace mongo {

    class BSONObj;
    class Status;

namespace repl {

    /**
     * Arguments to the replSetHeartbeat command.
     */
    class ReplSetHeartbeatArgs {
    public:
        /**
         * Initializes this ReplSetHeartbeatArgs from the contents of args.
         */
        Status initialize(const BSONObj& argsObj);

        /**
         * Returns whether the sender would like to know whether the node is empty or not.
         */
        bool getCheckEmpty() const { return _checkEmpty; }

        /**
         * Gets the version of the Heartbeat protocol being used by the sender.
         */
        long long getProtocolVersion() const { return _protocolVersion; }

        /**
         * Gets the ReplSetConfig version number of the sender.
         */ 
        long long getConfigVersion() const { return _configVersion; }

        /**
         * Gets the _id of the sender in their ReplSetConfig.
         */
        long long getSenderId() const { return _senderId; }

        /**
         * Gets the replSet name of the sender's replica set.
         */
        std::string getSetName() const { return _setName; }

        /**
         * Gets the HostAndPort of the sender.
         */
        HostAndPort getSenderHost() const { return _senderHost; }

    private:
        bool _checkEmpty;
        long long _protocolVersion;
        long long _configVersion;
        long long _senderId;
        std::string _setName;
        HostAndPort _senderHost;
    };

} // namespace repl
} // namespace mongo
