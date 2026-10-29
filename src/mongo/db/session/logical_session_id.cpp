// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/session/logical_session_id.h"

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

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
