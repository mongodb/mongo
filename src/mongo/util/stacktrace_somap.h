// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <string>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

class SharedObjectMapInfo {
public:
    explicit SharedObjectMapInfo(BSONObj obj);

    const BSONObj& obj() const;

    [[MONGO_MOD_PRIVATE]] void setObj(BSONObj obj);

private:
    BSONObj _obj;
};

// Can always be called, but more information is populated
// after the MONGO_INITIALIZER has run.
const SharedObjectMapInfo& globalSharedObjectMapInfo();

}  // namespace mongo
