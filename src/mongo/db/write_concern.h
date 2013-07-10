/**
 *    Copyright (C) 2013 10gen Inc.
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
 */

#pragma once

namespace mongo {

    /**
     * Helper method for commands to call.  Blocks until write concern (as specified in "cmdObj")
     * is satisfied.  "err" should be set to true if the last operation succeeded, otherwise false.
     * "result" will be filled with write concern results.  Returns false and sets "errmsg" on
     * failure.
     */
    bool waitForWriteConcern(const BSONObj& cmdObj,
                             bool err,
                             BSONObjBuilder* result,
                             string* errmsg);

} // namespace mongo
