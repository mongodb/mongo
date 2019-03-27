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

#pragma once

#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/optime.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/write_ops/batched_upsert_detail.h"
#include "mongo/s/write_ops/write_error_detail.h"

namespace mongo {

/**
 * This class represents the layout and content of a insert/update/delete runCommand,
 * the response side.
 */
class BatchedCommandResponse {
    BatchedCommandResponse(const BatchedCommandResponse&) = delete;
    BatchedCommandResponse& operator=(const BatchedCommandResponse&) = delete;

public:
    //
    // schema declarations
    //

    static const BSONField<long long> n;
    static const BSONField<long long> nModified;
    static const BSONField<std::vector<BatchedUpsertDetail*>> upsertDetails;
    static const BSONField<OID> electionId;
    static const BSONField<std::vector<WriteErrorDetail*>> writeErrors;
    static const BSONField<WriteConcernErrorDetail*> writeConcernError;

    BatchedCommandResponse();
    ~BatchedCommandResponse();
    BatchedCommandResponse(BatchedCommandResponse&&) = default;
    BatchedCommandResponse& operator=(BatchedCommandResponse&&) = default;

    bool isValid(std::string* errMsg) const;
    BSONObj toBSON() const;
    bool parseBSON(const BSONObj& source, std::string* errMsg);
    void clear();
    std::string toString() const;

    //
    // individual field accessors
    //

    /**
     * This group of getters/setters is only for the top-level command status. If you want to know
     * if all writes succeeded, use toStatus() below which considers all of the ways that writes can
     * fail.
     */
    void setStatus(Status status);
    Status getTopLevelStatus() const {
        dassert(_isStatusSet);
        return _status;
    }
    bool getOk() const {
        dassert(_isStatusSet);
        return _status.isOK();
    }

    void setNModified(long long n);
    void unsetNModified();
    bool isNModified() const;
    long long getNModified() const;

    void setN(long long n);
    void unsetN();
    bool isNSet() const;
    long long getN() const;

    void setUpsertDetails(const std::vector<BatchedUpsertDetail*>& upsertDetails);
    void addToUpsertDetails(BatchedUpsertDetail* upsertDetails);
    void unsetUpsertDetails();
    bool isUpsertDetailsSet() const;
    std::size_t sizeUpsertDetails() const;
    const std::vector<BatchedUpsertDetail*>& getUpsertDetails() const;
    const BatchedUpsertDetail* getUpsertDetailsAt(std::size_t pos) const;

    void setLastOp(repl::OpTime lastOp);
    void unsetLastOp();
    bool isLastOpSet() const;
    repl::OpTime getLastOp() const;

    void setElectionId(const OID& electionId);
    void unsetElectionId();
    bool isElectionIdSet() const;
    OID getElectionId() const;

    void setErrDetails(const std::vector<WriteErrorDetail*>& errDetails);
    // errDetails ownership is transferred to here.
    void addToErrDetails(WriteErrorDetail* errDetails);
    void unsetErrDetails();
    bool isErrDetailsSet() const;
    std::size_t sizeErrDetails() const;
    const std::vector<WriteErrorDetail*>& getErrDetails() const;
    const WriteErrorDetail* getErrDetailsAt(std::size_t pos) const;

    void setWriteConcernError(WriteConcernErrorDetail* error);
    void unsetWriteConcernError();
    bool isWriteConcernErrorSet() const;
    const WriteConcernErrorDetail* getWriteConcernError() const;

    /**
     * Converts the specified command response into a status, based on all of its contents.
     */
    Status toStatus() const;

private:
    // Convention: (M)andatory, (O)ptional

    // (M) The top-level command status.
    Status _status = Status::OK();
    bool _isStatusSet;

    // (M)  number of documents affected
    long long _n;
    bool _isNSet;

    // (O)  number of documents updated
    long long _nModified;
    bool _isNModifiedSet;

    // (O)  "promoted" _upserted, if the corresponding request contained only one batch item
    //      Should only be present if _upserted is not.
    BSONObj _singleUpserted;
    bool _isSingleUpsertedSet;

    // (O)  Array of upserted items' _id's
    //      Should only be present if _singleUpserted is not.
    std::unique_ptr<std::vector<BatchedUpsertDetail*>> _upsertDetails;

    // (O)  repl::OpTime assigned to the write op when it was written to the oplog.
    //      Normally, getLastError can use Client::_lastOp, but this is not valid for
    //      mongos which loses track of the session due to RCAR.  Therefore, we must
    //      keep track of the lastOp manually ourselves.
    repl::OpTime _lastOp;
    bool _isLastOpSet;

    // (O)  In addition to keeping track of the above lastOp repl::OpTime, we must also keep
    //      track of the primary we talked to.  This is because if the primary moves,
    //      subsequent calls to getLastError are invalid.  The only way we know if an
    //      election has occurred is to use the unique electionId.
    OID _electionId;
    bool _isElectionIdSet;

    // (O)  Array of item-level error information
    std::unique_ptr<std::vector<WriteErrorDetail*>> _writeErrorDetails;

    // (O)  errors that occurred while trying to satisfy the write concern.
    std::unique_ptr<WriteConcernErrorDetail> _wcErrDetails;
};

}  // namespace mongo
