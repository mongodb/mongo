/**
 *    Copyright (C) 2013 10gen Inc.
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

#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/optime.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/bson_serializable.h"
#include "mongo/s/write_ops/batched_upsert_detail.h"
#include "mongo/s/write_ops/write_error_detail.h"

namespace mongo {

/**
 * This class represents the layout and content of a insert/update/delete runCommand,
 * the response side.
 */
class BatchedCommandResponse : public BSONSerializable {

public:
    //
    // schema declarations
    //

    static const BSONField<int> ok;
    static const BSONField<int> errCode;
    static const BSONField<std::string> errMessage;
    static const BSONField<long long> n;
    static const BSONField<long long> nModified;
    static const BSONField<std::vector<BatchedUpsertDetail*>> upsertDetails;
    static const BSONField<OID> electionId;
    static const BSONField<std::vector<WriteErrorDetail*>> writeErrors;
    static const BSONField<WriteConcernErrorDetail*> writeConcernError;

    //
    // construction / destruction
    //

    BatchedCommandResponse();
    virtual ~BatchedCommandResponse();

    //
    // BatchedCommandResponse should have a move constructor but not a copy constructor
    //

    BatchedCommandResponse(BatchedCommandResponse&&) = default;
    BatchedCommandResponse& operator=(BatchedCommandResponse&&) = default;

    /** Copies all the fields present in 'this' to 'other'. */
    void cloneTo(BatchedCommandResponse* other) const;

    //
    // bson serializable interface implementation
    //

    virtual bool isValid(std::string* errMsg) const;
    virtual BSONObj toBSON() const;
    virtual bool parseBSON(const BSONObj& source, std::string* errMsg);
    virtual void clear();
    virtual std::string toString() const;

    //
    // individual field accessors
    //

    void setOk(int ok);
    int getOk() const;

    void setErrCode(int errCode);
    void unsetErrCode();
    bool isErrCodeSet() const;
    int getErrCode() const;

    void setErrMessage(StringData errMessage);
    bool isErrMessageSet() const;
    const std::string& getErrMessage() const;

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
     * Converts the specified command response into a status, based on its contents.
     */
    Status toStatus() const;

private:
    // Convention: (M)andatory, (O)ptional

    // (M)  0 if batch didn't get to be applied for any reason
    int _ok;
    bool _isOkSet;

    // (O)  whether all items in the batch applied correctly
    int _errCode;
    bool _isErrCodeSet;

    // (O)  whether all items in the batch applied correctly
    std::string _errMessage;
    bool _isErrMessageSet;

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
