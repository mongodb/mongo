// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/oplog_entry_serialization.h"

#include <charconv>
#include <string_view>

namespace mongo {
namespace repl {
void zeroOneManyStmtIdAppend(const std::vector<StmtId>& stmtIds,
                             std::string_view fieldName,
                             BSONObjBuilder* bob) {
    if (stmtIds.size() == 1) {
        bob->append(fieldName, stmtIds.front());
    } else if (stmtIds.size() > 1) {
        bob->append(fieldName, stmtIds);
    }
}

std::vector<StmtId> parseZeroOneManyStmtId(const BSONElement& element) {
    std::vector<StmtId> result;

    switch (element.type()) {
        case BSONType::numberInt:
            result.emplace_back(element._numberInt());
            break;
        case BSONType::array: {
            const BSONObj& arrayObject = element.Obj();
            std::uint32_t expectedFieldNumber = 0;
            for (const auto& arrayElement : arrayObject) {
                const auto& arrayFieldName = arrayElement.fieldNameStringData();
                std::uint32_t fieldNumber;
                auto fieldNameResult =
                    std::from_chars(arrayFieldName.data(),
                                    arrayFieldName.data() + arrayFieldName.size(),
                                    fieldNumber);
                uassert(8109802,
                        "Array field name is bogus",
                        fieldNameResult.ec == std::errc{} &&
                            fieldNameResult.ptr == arrayFieldName.data() + arrayFieldName.size() &&
                            fieldNumber == expectedFieldNumber++);

                uassert(8109801,
                        str::stream() << "Parsing stmtId, array element '" << arrayElement
                                      << "' is not valid.",
                        arrayElement.type() == BSONType::numberInt);
                result.emplace_back(arrayElement._numberInt());
            }
        } break;
        default:
            uasserted(8109800, str::stream() << "Field '" << element << "' is not valid.");
    }
    return result;
}

}  // namespace repl
}  // namespace mongo
