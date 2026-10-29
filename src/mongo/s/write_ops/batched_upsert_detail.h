// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <string>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * This class represents the layout and content of an item inside the 'upserted' array of a write
 * command's response (see BatchedCommandResponse).
 */
class BatchedUpsertDetail {
    BatchedUpsertDetail(const BatchedUpsertDetail&) = delete;
    BatchedUpsertDetail& operator=(const BatchedUpsertDetail&) = delete;

public:
    //
    // schema declarations
    //

    static const BSONField<int> index;
    static const BSONField<BSONObj> upsertedID;  // ID type

    //
    // construction / destruction
    //

    BatchedUpsertDetail();

    /** Copies all the fields present in 'this' to 'other'. */
    void cloneTo(BatchedUpsertDetail* other) const;

    //
    // bson serializable interface implementation
    //

    BSONObj toBSON() const;
    bool parseBSON(const BSONObj& source, std::string* errMsg);
    void clear();

    //
    // individual field accessors
    //

    void setIndex(int index);
    void unsetIndex();
    bool isIndexSet() const;
    int getIndex() const;

    void setUpsertedID(const BSONObj& upsertedID);
    void unsetUpsertedID();
    bool isUpsertedIDSet() const;
    const BSONObj& getUpsertedID() const;

private:
    // Convention: (M)andatory, (O)ptional

    // (M)  number of the batch item the upsert refers to
    int _index;
    bool _isIndexSet;

    // (M)  _id for the upserted document
    BSONObj _upsertedID;
    bool _isUpsertedIDSet;
};

}  // namespace mongo
