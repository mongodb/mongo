/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/index/column_key_generator.h"
#include "mongo/db/storage/column_store.h"

namespace mongo::sbe {

struct FakeCell {
    FakeCell(RecordId ridIn,
             std::string arrayInfoIn,
             std::vector<value::TypeTags> tagsIn,
             std::vector<value::Value> valuesIn,
             bool hasDuplicateFieldsIn,
             bool hasSubPathsIn,
             bool isSparseIn,
             bool hasDoubleNestedArraysIn)
        : rid(ridIn),
          arrayInfo(std::move(arrayInfoIn)),
          tags(std::move(tagsIn)),
          vals(std::move(valuesIn)),
          hasDuplicateFields(hasDuplicateFieldsIn),
          hasSubPaths(hasSubPathsIn),
          isSparse(isSparseIn),
          hasDoubleNestedArrays(hasDoubleNestedArraysIn) {}
    ~FakeCell() {
        for (size_t i = 0; i < tags.size(); ++i) {
            value::releaseValue(tags[i], vals[i]);
        }
    }
    FakeCell(const FakeCell&) = delete;
    FakeCell(FakeCell&&) = default;

    FakeCell& operator=(const FakeCell&) = delete;
    FakeCell& operator=(FakeCell&&) = default;


    // This really doesn't belong in the cell, but it's convenient to put here.
    RecordId rid;

    std::string arrayInfo;

    // Owned here.
    std::vector<value::TypeTags> tags;
    std::vector<value::Value> vals;

    bool hasDuplicateFields;
    bool hasSubPaths;
    bool isSparse;
    bool hasDoubleNestedArrays;
};

/**
 * Mock cursor for a single path in a column index. We use this instead of mocking the
 * ColumnStore::Cursor class because that allows traversing through the entire index, which
 * requires knowledge of all fields present across all documents.
 */
class FakeCursorForPath {
public:
    FakeCursorForPath(StringData path, std::unique_ptr<SeekableRecordCursor> cursor)
        : _path(path.toString()), _cursor(std::move(cursor)) {}
    boost::optional<FakeCell> next() {
        if (_eof)
            return {};
        return nextUntilResultIsFound(nullptr);
    }
    boost::optional<FakeCell> seekAtOrPast(RecordId rid) {
        auto result = _cursor->seekNear(rid);
        if (!result) {
            return {};
        }

        // seekNear() will return the key directionally _before_ the one requested if the requested
        // key does not exist.
        if (result->id < rid) {
            return nextUntilResultIsFound(nullptr);
        }

        return nextUntilResultIsFound(&*result);
    }
    boost::optional<FakeCell> seekExact(RecordId rid) {
        auto res = _cursor->seekExact(rid);
        if (res) {
            if (auto cell = extractCellForRecord(*res)) {
                return cell;
            }
        }
        return {};
    }

    void save() {
        if (_eof) {
            return saveUnpositioned();
        }
        _cursor->save();
    }
    void saveUnpositioned() {
        _eof = true;
        _cursor->saveUnpositioned();
    }

    void restore() {
        _cursor->restore();
    }

    void detachFromOperationContext() {
        _cursor->detachFromOperationContext();
    }
    void reattachToOperationContext(OperationContext* opCtx) {
        _cursor->reattachToOperationContext(opCtx);
    }

    const PathValue& path() const {
        return _path;
    }

private:
    boost::optional<FakeCell> extractCellForRecord(Record record) {
        boost::optional<FakeCell> ret;
        BSONObj bsonObj(record.data.data());

        column_keygen::visitCellsForInsert(
            bsonObj, [&](PathView path, const column_keygen::UnencodedCellView& cellView) {
                if (path != _path) {
                    return;
                }

                std::vector<value::TypeTags> tags;
                std::vector<value::Value> vals;

                for (auto elem : cellView.vals) {
                    auto [t, v] = bson::convertFrom<false>(elem);
                    tags.push_back(t);
                    vals.push_back(v);
                }

                ret.emplace(record.id,
                            cellView.arrayInfo.toString(),
                            std::move(tags),
                            std::move(vals),
                            cellView.hasDuplicateFields,
                            cellView.hasSubPaths,
                            cellView.isSparse,
                            cellView.hasDoubleNestedArrays);
            });
        return ret;
    }

    /*
     * Iterates the underlying cursor until a result is found which contains _path and can be
     * returned.
     *
     * If 'startRecord' is not null, it is assumed that the cursor is positioned at 'startRecord'
     * and 'startRecord' will be considered as a possible record to return data from.  Otherwise,
     * the first record considered will be the next result from the cursor.
     */
    boost::optional<FakeCell> nextUntilResultIsFound(Record* startRecord) {
        boost::optional<Record> currentRecord;
        if (startRecord) {
            currentRecord = *startRecord;
        } else {
            currentRecord = _cursor->next();
        }

        while (currentRecord) {
            if (auto ret = extractCellForRecord(*currentRecord)) {
                return ret;
            }
            currentRecord = _cursor->next();
        }

        return boost::none;
    }
    const PathValue _path;
    bool _eof = false;
    const std::unique_ptr<SeekableRecordCursor> _cursor;
};
}  // namespace mongo::sbe
