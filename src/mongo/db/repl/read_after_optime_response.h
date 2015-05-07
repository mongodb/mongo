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

#include <string>

#include "mongo/base/status.h"
#include "mongo/stdx/chrono.h"

namespace mongo {

    class BSONObjBuilder;

namespace repl {

    class ReadAfterOpTimeResponse {
    public:
        static const std::string kWaitedMSFieldName;

        /**
         * Constructs a default response that has OK status, and wait is false.
         */
        ReadAfterOpTimeResponse();

        /**
         * Constructs a response with the given status with wait equals to false.
         */
        explicit ReadAfterOpTimeResponse(Status status);

        /**
         * Constructs a response with wait set to true along with the given parameters.
         */
        ReadAfterOpTimeResponse(Status status, stdx::chrono::milliseconds duration);

        /**
         * Appends to the builder the timeout and duration info if didWait() is true.
         * Note: does not include status.
         */
        void appendInfo(BSONObjBuilder* builder);

        bool didWait() const;

        /**
         * Returns the amount of duration waiting for opTime to pass.
         * Valid only if didWait is true.
         */
        stdx::chrono::milliseconds getDuration() const;

        /**
         * Returns more details about an error if it occurred.
         */
        Status getStatus() const;

    private:
        ReadAfterOpTimeResponse(Status status,
                                stdx::chrono::milliseconds duration,
                                bool waited);

        bool _waited;
        stdx::chrono::milliseconds _duration;
        Status _status;
    };

} // namespace repl
} // namespace mongo
