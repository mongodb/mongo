// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>

#include <boost/cstdint.hpp>
#include <boost/functional/hash.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

using TxnNumber = std::int64_t;
using StmtId = std::int32_t;
using TxnRetryCounter = std::int32_t;

// Default value for unassigned statementId.
const StmtId kUninitializedStmtId = -1;

// Used as a substitute statementId for oplog entries that were truncated and lost.
const StmtId kIncompleteHistoryStmtId = -2;

const TxnNumber kUninitializedTxnNumber = -1;
const TxnRetryCounter kUninitializedTxnRetryCounter = -1;

class BSONObjBuilder;
class OperationContext;

inline bool operator==(const LogicalSessionId& lhs, const LogicalSessionId& rhs) {
    return (lhs.getId() == rhs.getId()) && (lhs.getTxnNumber() == rhs.getTxnNumber()) &&
        (lhs.getTxnUUID() == rhs.getTxnUUID()) && (lhs.getUid() == rhs.getUid());
}

inline bool operator!=(const LogicalSessionId& lhs, const LogicalSessionId& rhs) {
    return !(lhs == rhs);
}

inline bool operator==(const LogicalSessionRecord& lhs, const LogicalSessionRecord& rhs) {
    return lhs.getId() == rhs.getId();
}

inline bool operator!=(const LogicalSessionRecord& lhs, const LogicalSessionRecord& rhs) {
    return !(lhs == rhs);
}

struct LogicalSessionIdHash {
    std::size_t operator()(const LogicalSessionId& lsid) const {
        // Due to internal sessions having the same _id, we want to hash by its txnUUID and
        // txnNumber to discourage hash key collision.
        if (auto txnUUID = lsid.getTxnUUID()) {
            auto hash = _hasher(*txnUUID);
            if (lsid.getTxnNumber()) {
                boost::hash_combine(hash, *lsid.getTxnNumber());
            }
            return hash;
        }
        return _hasher(lsid.getId());
    }

private:
    UUID::Hash _hasher;
};

struct LogicalSessionRecordHash {
    std::size_t operator()(const LogicalSessionRecord& lsid) const {
        return _hasher(lsid.getId());
    }

private:
    LogicalSessionIdHash _hasher;
};

inline std::ostream& operator<<(std::ostream& s, const LogicalSessionId& lsid) {
    return (s << lsid.getId() << " - " << lsid.getUid() << " - "
              << (lsid.getTxnNumber() ? std::to_string(*lsid.getTxnNumber()) : "") << " - "
              << (lsid.getTxnUUID() ? lsid.getTxnUUID()->toString() : ""));
}

inline StringBuilder& operator<<(StringBuilder& s, const LogicalSessionId& lsid) {
    return (s << lsid.getId().toString() << " - " << lsid.getUid().toString() << " - "
              << (lsid.getTxnNumber() ? std::to_string(*lsid.getTxnNumber()) : "") << " - "
              << (lsid.getTxnUUID() ? lsid.getTxnUUID()->toString() : ""));
}

inline std::ostream& operator<<(std::ostream& s, const LogicalSessionFromClient& lsid) {
    return (s << lsid.getId() << " - " << (lsid.getUid() ? lsid.getUid()->toString() : "") << " - "
              << (lsid.getTxnNumber() ? std::to_string(*lsid.getTxnNumber()) : "") << " - "
              << (lsid.getTxnUUID() ? lsid.getTxnUUID()->toString() : ""));
}

inline StringBuilder& operator<<(StringBuilder& s, const LogicalSessionFromClient& lsid) {
    return (s << lsid.getId() << " - " << (lsid.getUid() ? lsid.getUid()->toString() : "") << " - "
              << (lsid.getTxnNumber() ? std::to_string(*lsid.getTxnNumber()) : "") << " - "
              << (lsid.getTxnUUID() ? lsid.getTxnUUID()->toString() : ""));
}

/**
 * An alias for sets of session ids.
 */
using LogicalSessionIdSet = stdx::unordered_set<LogicalSessionId, LogicalSessionIdHash>;
using LogicalSessionRecordSet = stdx::unordered_set<LogicalSessionRecord, LogicalSessionRecordHash>;

template <typename T>
using LogicalSessionIdMap = stdx::unordered_map<LogicalSessionId, T, LogicalSessionIdHash>;

class TxnNumberAndRetryCounter {
public:
    TxnNumberAndRetryCounter(TxnNumber txnNumber, boost::optional<TxnRetryCounter> txnRetryCounter)
        : _txnNumber(txnNumber), _txnRetryCounter(txnRetryCounter) {}

    TxnNumberAndRetryCounter(TxnNumber txnNumber)
        : _txnNumber(txnNumber), _txnRetryCounter(boost::none) {}

    BSONObj toBSON() const {
        BSONObjBuilder bob;
        bob.append(OperationSessionInfoFromClientBase::kTxnNumberFieldName, _txnNumber);
        if (_txnRetryCounter) {
            bob.append(OperationSessionInfoFromClientBase::kTxnRetryCounterFieldName,
                       *_txnRetryCounter);
        }
        return bob.obj();
    }

    TxnNumber getTxnNumber() const {
        return _txnNumber;
    }

    boost::optional<TxnRetryCounter> getTxnRetryCounter() const {
        return _txnRetryCounter;
    }

    void setTxnNumber(const TxnNumber txnNumber) {
        _txnNumber = txnNumber;
    }

    void setTxnRetryCounter(const boost::optional<TxnRetryCounter> txnRetryCounter) {
        _txnRetryCounter = txnRetryCounter;
    }

private:
    TxnNumber _txnNumber;
    boost::optional<TxnRetryCounter> _txnRetryCounter;
};

inline bool operator==(const TxnNumberAndRetryCounter& l, const TxnNumberAndRetryCounter& r) {
    return l.getTxnNumber() == r.getTxnNumber() && l.getTxnRetryCounter() == r.getTxnRetryCounter();
}

inline bool operator!=(const TxnNumberAndRetryCounter& l, const TxnNumberAndRetryCounter& r) {
    return !(l == r);
}

/**
 * Represents all the session-related state that a client can attach when invoking a command against
 * the server. Clients are allowed to invoke commands without attaching a session in which case the
 * default-constructed value means "no logical session". However, if a client sends a session, they
 * must specify at least the "lsid" field.
 */
class OperationSessionInfoFromClient : public OperationSessionInfoFromClientBase {
public:
    /**
     * Returns a default-constructed object meaning "there is no logical session".
     */
    OperationSessionInfoFromClient() = default;
    OperationSessionInfoFromClient(LogicalSessionFromClient lsidFromClient);

    OperationSessionInfoFromClient(LogicalSessionId lsid, boost::optional<TxnNumber> txnNumber);

    explicit OperationSessionInfoFromClient(OperationSessionInfoFromClientBase other)
        : OperationSessionInfoFromClientBase(std::move(other)) {}
};

}  // namespace mongo
