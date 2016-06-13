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
#include <vector>

#include "mongo/bson/oid.h"

namespace mongo {
struct HostAndPort;
class ServiceContext;

namespace repl {

/**
 * An identifier unique to this instance. Used by isSelf to see if we are talking
 * to ourself or someone else.
 */
extern OID instanceId;

/**
 * Returns true if "hostAndPort" identifies this instance.
 */
bool isSelf(const HostAndPort& hostAndPort, ServiceContext* ctx);

/**
 * Returns all the IP addresses bound to the network interfaces of this machine.
 * This requires a syscall. If the ipv6enabled parameter is true, both IPv6 AND IPv4
 * addresses will be returned.
 *
 * Note: this only works on Linux and Windows. All calls should be properly ifdef'd,
 * otherwise an invariant will be triggered.
 */
std::vector<std::string> getBoundAddrs(const bool ipv6enabled);

}  // namespace repl
}  // namespace mongo
