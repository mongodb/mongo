/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/session/logical_session_id.h"

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

LogicalSessionId makeLogicalSessionIdForTest() {
    LogicalSessionId lsid;

    lsid.setId(UUID::gen());
    lsid.setUid(SHA256Block::computeHash({}));

    return lsid;
}

LogicalSessionId makeLogicalSessionIdWithTxnNumberAndUUIDForTest(
    boost::optional<LogicalSessionId> parentLsid, boost::optional<TxnNumber> parentTxnNumber) {
    auto lsid = parentLsid ? LogicalSessionId(parentLsid->getId(), parentLsid->getUid())
                           : makeLogicalSessionIdForTest();
    lsid.setTxnUUID(UUID::gen());
    lsid.setTxnNumber(parentTxnNumber ? *parentTxnNumber : 0);
    return lsid;
}

LogicalSessionId makeLogicalSessionIdWithTxnUUIDForTest(
    boost::optional<LogicalSessionId> parentLsid) {
    auto lsid = parentLsid ? LogicalSessionId(parentLsid->getId(), parentLsid->getUid())
                           : makeLogicalSessionIdForTest();
    lsid.setTxnUUID(UUID::gen());
    return lsid;
}

LogicalSessionRecord makeLogicalSessionRecordForTest() {
    LogicalSessionRecord record{};

    record.setId(makeLogicalSessionIdForTest());

    return record;
}

OperationSessionInfoFromClient::OperationSessionInfoFromClient(
    LogicalSessionFromClient lsidFromClient) {
    setSessionId(std::move(lsidFromClient));
}

OperationSessionInfoFromClient::OperationSessionInfoFromClient(LogicalSessionId lsid,
                                                               boost::optional<TxnNumber> txnNumber)
    : OperationSessionInfoFromClient([&] {
          LogicalSessionFromClient lsidFromClient(lsid.getId());
          lsidFromClient.setUid(lsid.getUid());
          lsidFromClient.setTxnNumber(lsid.getTxnNumber());
          lsidFromClient.setTxnUUID(lsid.getTxnUUID());
          return lsidFromClient;
      }()) {
    setTxnNumber(std::move(txnNumber));
}

}  // namespace mongo
