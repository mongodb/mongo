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

#include "mongo/platform/basic.h"

#include <utility>

#include "mongo/db/dbmessage.h"
#include "mongo/rpc/legacy_request.h"
#include "mongo/rpc/metadata.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace rpc {

OpMsgRequest opMsgRequestFromLegacyRequest(const Message& message) {
    DbMessage dbm(message);
    QueryMessage qm(dbm);
    NamespaceString ns(qm.ns);

    uassert(40473,
            str::stream() << "Trying to handle namespace " << qm.ns << " as a command",
            ns.isCommand());

    uassert(16979,
            str::stream() << "bad numberToReturn (" << qm.ntoreturn
                          << ") for $cmd type ns - can only be 1 or -1",
            qm.ntoreturn == 1 || qm.ntoreturn == -1);

    auto bodyAndMetadata = rpc::upconvertRequestMetadata(qm.query, qm.queryOptions);

    return OpMsgRequest::fromDBAndBody(
        ns.db(), std::move(std::get<0>(bodyAndMetadata)), std::get<1>(bodyAndMetadata));
}

}  // namespace rpc
}  // namespace mongo
