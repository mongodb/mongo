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

namespace mongo {
class BSONObj;

/**
 * @return true if internal authentication parameters has been set up. Note this does not
 * imply that auth is enabled. For instance, with the --transitionToAuth flag this will
 * be set and auth will be disabled.
 */
bool isInternalAuthSet();

/**
 * This method initializes the authParams object with authentication
 * credentials to be used by authenticateInternalUser.
 */
void setInternalUserAuthParams(const BSONObj& authParamsIn);

/**
 * Returns a copy of the authParams object to be used by authenticateInternalUser
 *
 * The format of the return object is { authparams, fallbackParams:params}
 *
 * If SCRAM-SHA-1 is the internal auth mechanism the fallbackParams sub document is
 * for MONGODB-CR auth is included. For MONGODB-XC509 no fallbackParams document is
 * returned.
 **/
BSONObj getInternalUserAuthParamsWithFallback();
}  // namespace mongo
