// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <utility>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * This class represents the layout and content of the error that occurs while trying
 * to satisfy the write concern after executing runCommand.
 */
class WriteConcernErrorDetail {
public:
    WriteConcernErrorDetail();
    WriteConcernErrorDetail(Status status);

    /** Copies all the fields present in 'this' to 'other'. */
    void cloneTo(WriteConcernErrorDetail* other) const;

    //
    // bson serializable interface implementation
    //

    bool isValid(std::string* errMsg) const;
    BSONObj toBSON() const;
    bool parseBSON(const BSONObj& source, std::string* errMsg);
    void clear();
    std::string toString() const;

    //
    // individual field accessors
    //

    void setStatus(Status status) {
        _status = std::move(status);
    }

    Status toStatus() const;

    void setErrInfo(const BSONObj& errInfo);
    bool isErrInfoSet() const;
    const BSONObj& getErrInfo() const;

private:
    // Convention: (M)andatory, (O)ptional

    // (M)  error code and message. Must be set to a not OK status to be valid.
    Status _status = Status::OK();

    // (O)  further details about the write concern error.
    BSONObj _errInfo;
    bool _isErrInfoSet;
};

/**
 * Creates and returns a WriteConcernErrorDetail object from a BSONObj.
 */
std::unique_ptr<WriteConcernErrorDetail> getWriteConcernErrorDetailFromBSONObj(const BSONObj& obj);

/**
 * Constructs a WriteConcernErrorDetail by parsing the given BSONElement.
 */
WriteConcernErrorDetail getWriteConcernErrorDetail(const BSONElement& wcErrorElem);

}  // namespace mongo
