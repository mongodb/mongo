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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <boost/optional.hpp>

#include "mongo/bson/bsonobj.h"

namespace mongo {

class BSONObjBuilder;
template <typename T>
class StatusWith;
struct WriteConcernOptions;

/**
 * Returns the default write concern for migration cleanup on the donor shard and for cloning
 * documents on the destination shard.
 */
class MigrationSecondaryThrottleOptions {
public:
    enum SecondaryThrottleOption {
        // The secondary throttle option is not set explicitly. Use the default for the service.
        kDefault,

        // The secondary throttle option is explicitly disabled.
        kOff,

        // The secondary throttle option is explicitly enabled and there potentially might be a
        // write concern specified. If no write concern was specified, use the default.
        kOn
    };

    /**
     * Constructs an object with the specified secondary throttle option and no custom write
     * concern.
     */
    static MigrationSecondaryThrottleOptions create(SecondaryThrottleOption option);

    /**
     * Constructs an object with the secondary throttle enabled and with the specified write
     * concern.
     */
    static MigrationSecondaryThrottleOptions createWithWriteConcern(
        const WriteConcernOptions& writeConcern);

    /**
     * Extracts the write concern settings from a BSON with the following format:
     *
     * {
     *     secondaryThrottle: <bool>,                           // optional
     *     _secondaryThrottle: <bool>,                          // optional
     *     writeConcern: <WriteConcern formatted as BSONObj>    // optional
     * }
     *
     * Note: secondaryThrottle takes precedence over _secondaryThrottle. If either of the two are
     * missing, the secondaryThrottle enabled status defaults to true.
     *
     * Returns OK if the parse was successful.
     */
    static StatusWith<MigrationSecondaryThrottleOptions> createFromCommand(const BSONObj& obj);

    /**
     * Extracts the secondary throttle settings from a balancer configuration document, which can
     * have the following format:
     *
     * {
     *     _secondaryThrottle: <bool> | <WriteConcern formatted as BSONObj> // optional
     * }
     *
     * If secondary throttle is not specified, uses kDefault.
     */
    static StatusWith<MigrationSecondaryThrottleOptions> createFromBalancerConfig(
        const BSONObj& obj);

    /**
     * Returns the selected secondary throttle option.
     */
    SecondaryThrottleOption getSecondaryThrottle() const {
        return _secondaryThrottle;
    }

    /**
     * Returns whether secondary throttle is enabled and write concern was requested.
     */
    bool isWriteConcernSpecified() const {
        return _writeConcernBSON.is_initialized();
    }

    /**
     * Returns the custom write concern, which was requested. Must only be called if
     * isWriteConcernSpecified returns true.
     */
    WriteConcernOptions getWriteConcern() const;

    /**
     * Returns a BSON representation of the current secondary throttle settings.
     */
    void append(BSONObjBuilder* builder) const;
    BSONObj toBSON() const;

    /**
     * Returns true if the options match exactly.
     */
    bool operator==(const MigrationSecondaryThrottleOptions& other) const;
    bool operator!=(const MigrationSecondaryThrottleOptions& other) const;

private:
    MigrationSecondaryThrottleOptions(SecondaryThrottleOption secondaryThrottle,
                                      boost::optional<BSONObj> writeConcernBSON);

    // What is the state of the secondaryThrottle option (kDefault means that it has not been set)
    SecondaryThrottleOption _secondaryThrottle;

    // Owned BSON object with the contents of the writeConcern. If this object is set, then
    // secodaryThrottle must be true.
    boost::optional<BSONObj> _writeConcernBSON;
};

}  // namespace mongo
