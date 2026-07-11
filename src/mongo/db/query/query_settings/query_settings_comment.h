// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/basic_types.h"
#include "mongo/util/modules.h"

#include <compare>
#include <string_view>

namespace mongo::query_settings {

class Comment {
public:
    static Comment parseFromBSON(const BSONElement& element) {
        return Comment(element);
    }

    Comment(const BSONElement& element) {
        _comment = IDLAnyTypeOwned(element);
    }

    void serializeToBSON(std::string_view fieldName, BSONObjBuilder* builder) const {
        _comment.serializeToBSON(fieldName, builder);
    }

    void serializeToBSON(BSONArrayBuilder* builder) const {
        _comment.serializeToBSON(builder);
    }

    const BSONElement& getElement() const {
        return _comment.getElement();
    }

    std::strong_ordering operator<=>(const Comment& other) const;
    bool operator==(const Comment& other) const;

private:
    IDLAnyTypeOwned _comment;
};

}  // namespace mongo::query_settings
