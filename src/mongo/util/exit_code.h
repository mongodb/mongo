/** @file mongo/util/exit_code.h
 *
 * Mongo exit codes.
 */

/*    Copyright 2009 10gen Inc.
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

namespace mongo {

enum ExitCode : int {
    EXIT_CLEAN = 0,
    EXIT_BADOPTIONS = 2,
    EXIT_REPLICATION_ERROR = 3,
    EXIT_NEED_UPGRADE = 4,
    EXIT_SHARDING_ERROR = 5,
    EXIT_KILL = 12,
    EXIT_ABRUPT = 14,
    EXIT_NTSERVICE_ERROR = 20,
    EXIT_JAVA = 21,
    EXIT_OOM_MALLOC = 42,
    EXIT_OOM_REALLOC = 43,
    EXIT_FS = 45,
    EXIT_CLOCK_SKEW = 47,  // OpTime clock skew, deprecated
    EXIT_NET_ERROR = 48,
    EXIT_WINDOWS_SERVICE_STOP = 49,
    EXIT_POSSIBLE_CORRUPTION =
        60,  // this means we detected a possible corruption situation, like a buf overflow
    EXIT_UNCAUGHT = 100,  // top level exception that wasn't caught
    EXIT_TEST = 101
};

}  // namespace mongo
