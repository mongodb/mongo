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

#include <cstdint>
#include <string>
#include <type_traits>

#include "mongo/base/status_with.h"

namespace mongo {
class BSONObj;
class OperationContext;
namespace rpc {

/**
 * Bit flags representing support for a particular RPC protocol.
 * This is just an internal representation, and is never transmitted over the wire. It should
 * never be used for any other feature detection in favor of max/min wire version.
 *
 * A new protocol must be added as the highest order bit flag so that it is prioritized in
 * negotiation.
 */
enum class Protocol : std::uint64_t {

    /**
     * The pre-3.2 OP_QUERY on db.$cmd protocol
     */
    kOpQuery = 1 << 0,

    /**
     * The post-3.2 OP_COMMAND protocol.
     */
    kOpCommandV1 = 1 << 1,
};

/**
 * Bitfield representing a set of supported RPC protocols.
 */
using ProtocolSet = std::underlying_type<Protocol>::type;

/**
 * This namespace contains predefined bitfields for common levels of protocol support.
 */
namespace supports {

const ProtocolSet kNone = ProtocolSet{0};
const ProtocolSet kOpQueryOnly = static_cast<ProtocolSet>(Protocol::kOpQuery);
const ProtocolSet kOpCommandOnly = static_cast<ProtocolSet>(Protocol::kOpCommandV1);
const ProtocolSet kAll = kOpQueryOnly | kOpCommandOnly;

}  // namespace supports

/**
 * Returns the protocol used to initiate the current operation.
 */
Protocol getOperationProtocol(OperationContext* txn);

/**
 * Sets the protocol used to initiate the current operation.
 */
void setOperationProtocol(OperationContext* txn, Protocol protocol);

/**
 * Returns the newest protocol supported by two parties.
 */
StatusWith<Protocol> negotiate(ProtocolSet fst, ProtocolSet snd);

/**
 * Converts a ProtocolSet to a string. Currently only the predefined ProtocolSets in the
 * 'supports' namespace are supported.
 *
 * This intentionally does not conform to the STL 'to_string' convention so that it will
 * not conflict with the to_string overload for uint64_t.
 */
StatusWith<StringData> toString(ProtocolSet protocols);

/**
 * Parses a ProtocolSet from a string. Currently only the predefined ProtocolSets in the
 * 'supports' namespace are supported
 */
StatusWith<ProtocolSet> parseProtocolSet(StringData repr);

/**
 * Determines the ProtocolSet of a remote server from an isMaster reply.
 */
StatusWith<ProtocolSet> parseProtocolSetFromIsMasterReply(const BSONObj& isMasterReply);

/**
 * Returns true if wire version supports OP_COMMAND in mongod (not mongos).
 */
bool supportsWireVersionForOpCommandInMongod(int minWireVersion, int maxWireVersion);

/**
  * Computes supported protocols from wire versions.
  */
ProtocolSet computeProtocolSet(int minWireVersion, int maxWireVersion);

}  // namespace rpc
}  // namespace mongo
