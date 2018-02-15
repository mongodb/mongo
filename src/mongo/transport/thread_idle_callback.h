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

namespace mongo {

/**
 * Type of callback functions that can be invoked when markThreadIdle() runs. These functions *must
 * not throw*.
 */
typedef void (*ThreadIdleCallback)();

/**
 * Informs the registered listener that this thread believes it may go idle for an extended period.
 * The caller should avoid calling markThreadIdle at a high rate, as it can both be moderately
 * costly itself and in terms of distributed overhead for subsequent malloc/free calls.
 */
void markThreadIdle();

/**
 * Allows for registering callbacks for when threads go idle and become active. This is used by
 * TCMalloc to return freed memory to its central freelist at appropriate points, so it won't happen
 * during critical sections while holding locks. Calling this is not thread-safe.
 */
void registerThreadIdleCallback(ThreadIdleCallback callback);

}  // namespace mongo
