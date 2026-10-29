// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {

class SharedObjectMapInfo {
public:
    explicit SharedObjectMapInfo(BSONObj obj);

    const BSONObj& obj() const;

    void setObj(BSONObj obj);

private:
    BSONObj _obj;
};

// Can always be called, but more information is populated
// after the MONGO_INITIALIZER has run.
const SharedObjectMapInfo& globalSharedObjectMapInfo();

}  // namespace mongo
