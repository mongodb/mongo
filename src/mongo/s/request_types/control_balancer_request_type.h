/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/base/string_data.h"

namespace mongo {

class BSONObj;
template <typename T>
class StatusWith;

/**
 * Specifies a representation of the external mongos and internal config versions of the
 * controlBalancer command, and provides methods to convert the representation to and from BSON.
 */
class ControlBalancerRequest {
public:
    // The requested balancer control action
    enum BalancerControlAction {
        // Start the balancer
        kStart,

        // Stop the balancer
        kStop,
    };

    /**
     * Parses the specified BSON object as a mongos command and extracts the requested balancer
     * control action or returns an error.
     */
    static StatusWith<ControlBalancerRequest> parseFromMongosCommand(const BSONObj& obj);

    /**
     * Parses the specified BSON object as a config server command and extracts the requested
     * balancer control action or returns an error.
     */
    static StatusWith<ControlBalancerRequest> parseFromConfigCommand(const BSONObj& obj);

    BalancerControlAction getAction() const {
        return _action;
    }

    /**
     * Returns a BSON representation of this request, which can be used for sending to the config
     * server.
     */
    BSONObj toCommandForConfig() const;

private:
    explicit ControlBalancerRequest(BalancerControlAction action);

    /**
     * Common method to extract the balancer control action based on the mongos or the config server
     * command.
     */
    static StatusWith<ControlBalancerRequest> _parse(const BSONObj& obj, StringData commandString);

    BalancerControlAction _action;
};

}  // namespace mongo
