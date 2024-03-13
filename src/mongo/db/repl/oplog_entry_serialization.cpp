/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/repl/oplog_entry_serialization.h"

namespace mongo {
namespace repl {
void zeroOneManyStmtIdAppend(const std::vector<StmtId>& stmtIds,
                             StringData fieldName,
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
        case NumberInt:
            result.emplace_back(element._numberInt());
            break;
        case Array: {
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
                            fieldNameResult.ptr ==
                                arrayFieldName.rawData() + arrayFieldName.size() &&
                            fieldNumber == expectedFieldNumber++);

                uassert(8109801,
                        str::stream() << "Parsing stmtId, array element '" << arrayElement
                                      << "' is not valid.",
                        arrayElement.type() == NumberInt);
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
