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

#include <boost/optional/optional.hpp>
#include <stack>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/catalog/validate_results.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/ident.h"

namespace mongo {
using PathView = StringData;
using PathValue = std::string;
using CellView = StringData;
using CellValue = std::string;
using RowId = int64_t;

struct FullCellView {
    PathView path;
    RowId rid;
    CellView value;
};

/**
 * An owned representation of a column-store index entry cell.
 */
struct FullCellValue {
    PathValue path;
    RowId rid;
    CellValue value;

    FullCellValue(FullCellView fcv) : path(fcv.path), rid(fcv.rid), value(fcv.value) {}
    FullCellValue(PathView p, RowId r, CellView v) : path(p), rid(r), value(v) {}
};

struct CellViewForPath {
    RowId rid;
    CellView value;
};

/**
 * This is a base class for column store index storage access, similar to RecordStore and
 * SortedDataInterface.
 *
 * Whereas RecordStore maps RecordId to RecordData and SDI maps KeyString to RecordId, ColumnStore
 * stores tuples of Path, RecordId and Value in a separate format.
 */
class ColumnStore {
public:
    class Cursor;
    class WriteCursor {
    public:
        virtual ~WriteCursor() = default;
        virtual void insert(PathView, RowId, CellView) = 0;
        virtual void remove(PathView, RowId) = 0;
        virtual void update(PathView, RowId, CellView) = 0;
    };

    /**
     * Wrapper around a regular storage cursor to return results only for a specific data column in
     * a column store index. Only returns documents matching the path specified in the constructor.
     * Considers non-path matching documents EOF and returns boost::none.
     */
    class ColumnCursor {
    public:
        ColumnCursor(StringData path, std::unique_ptr<Cursor> cursor)
            : _path(path.toString()),
              _numPathParts(FieldRef{_path}.numParts()),
              _cursor(std::move(cursor)) {}

        boost::optional<FullCellView> next() {
            if (_eof)
                return {};
            return handleResult(_cursor->next());
        }
        boost::optional<FullCellView> seekAtOrPast(RowId rid) {
            return handleResult(_cursor->seekAtOrPast(_path, rid));
        }
        boost::optional<FullCellView> seekExact(RowId rid) {
            return handleResult(_cursor->seekExact(_path, rid));
        }

        void save() {
            if (_eof)
                return saveUnpositioned();
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

        FieldIndex numPathParts() const {
            return _numPathParts;
        }

    private:
        boost::optional<FullCellView> handleResult(boost::optional<FullCellView> res) {
            if (!res || res->path != _path) {
                _eof = true;
                return {};
            } else if (_eof) {
                _eof = false;
            }
            return res;
        }
        const PathValue _path;
        const FieldIndex _numPathParts = 0;
        bool _eof = true;
        const std::unique_ptr<Cursor> _cursor;
    };

    class BulkBuilder {
    public:
        virtual ~BulkBuilder() = default;
        virtual void addCell(PathView, RowId, CellView) = 0;
    };

    /**
     * This reserved "path" is used for keeping track of all RecordIds in the collection. Cells at
     * this path should always have an empty CellView to ensure the most compact representation for
     * this subtree.
     * This is not a valid real path because it can never appear in valid UTF-8 data.
     */
    static constexpr StringData kRowIdPath = "\xFF"_sd;

    // RowId equivalent of a null RecordId
    static const RowId kNullRowId = 0;

    // This is really just a namespace
    struct Bytes {
        static constexpr uint8_t kFirstNonBson = 0x20;
        static_assert(kFirstNonBson > BSONType::JSTypeMax);  // Have room for 12 new types.

        // no-value types
        static constexpr uint8_t kNull = 0x20;
        static constexpr uint8_t kMinKey = 0x21;
        static constexpr uint8_t kMaxKey = 0x22;

        // Bool (value encoded in this byte)
        static constexpr uint8_t kFalse = 0x23;
        static constexpr uint8_t kTrue = 0x24;

        // Empty Object and Array (value encoded in this byte)
        static constexpr uint8_t kEmptyObj = 0x25;
        static constexpr uint8_t kEmptyArr = 0x26;

        static constexpr uint8_t kOID = 0x27;   // 12 bytes follow
        static constexpr uint8_t kUUID = 0x28;  // 16 bytes follow (newUUID subtype)

        // Gap from 0x29 - 0x2f (room for more simple types and more encodings of Decimal128)

        static constexpr uint8_t kDecimal128 = 0x30;  // 16 bytes follow

        // NumberDouble
        static constexpr uint8_t kDouble = 0x31;       // 8 bytes follow
        static constexpr uint8_t kShortDouble = 0x32;  // 4 bytes follow (when float(x) == x)
        // 0x33 and 0x34 are reserved for bfloat16 (truncated single) and IEEE754 float16.
        static constexpr uint8_t kInt1Double = 0x35;  // 1 bytes follow (when int8_t(x) == x)

        // NumberDouble when (100 * x) can safely be represented as an integer
        static constexpr uint8_t kCents1Double = 0x36;  // 1 byte follows
        static constexpr uint8_t kCents2Double = 0x37;  // 2 bytes follow
        static constexpr uint8_t kCents4Double = 0x38;  // 4 bytes follow

        // NumberInt (N bytes follow)
        static constexpr uint8_t kInt1 = 0x39;
        static constexpr uint8_t kInt2 = 0x3a;
        static constexpr uint8_t kInt4 = 0x3b;

        // NumberLong (N bytes follow)
        static constexpr uint8_t kLong1 = 0x3c;
        static constexpr uint8_t kLong2 = 0x3d;
        static constexpr uint8_t kLong4 = 0x3e;
        static constexpr uint8_t kLong8 = 0x3f;

        // These encode small Int and Long directly in this byte
        static constexpr uint8_t kTinyIntMin = 0x40;
        static constexpr uint8_t kTinyIntMax = 0x5f;
        static constexpr uint8_t kTinyLongMin = 0x60;
        static constexpr uint8_t kTinyLongMax = 0x7f;

        // String (N - kStringSizeMin bytes follow)
        static constexpr uint8_t kStringSizeMin = 0x80;
        static constexpr uint8_t kStringSizeMax = 0xc0;

        // Gap from 0xc1 - 0xcf

        // Bytes here or above indicate prefix data before the data. Any byte below this is the
        // start of data. Prefix data is all optional, but when present, must be in this order:
        //   - kSubPathsMarker
        //   - kSparseMarker
        //   - kDoubleNestedArraysMarker
        //   - kArrInfoSizeXXX
        static constexpr uint8_t kFirstPrefixByte = 0xd0;

        static constexpr uint8_t kFirstArrInfoSize = 0xd0;
        // Directly encode number of bytes at end of cell
        static constexpr uint8_t kArrInfoSizeTinyMin = 0xd0;  // Note that this means 1 byte stored
        static constexpr uint8_t kArrInfoSizeTinyMax = 0xec;

        // N bytes of ArrInfo at end of Cell.
        static constexpr uint8_t kArrInfoSize1 = 0xed;
        static constexpr uint8_t kArrInfoSize2 = 0xee;
        static constexpr uint8_t kArrInfoSize4 = 0xef;
        static constexpr uint8_t kLastArrInfoSize = 0xef;

        // Gap from 0xf0 - 0xfb

        static constexpr uint8_t kDuplicateFieldsMarker = 0xfc;
        static constexpr uint8_t kSubPathsMarker = 0xfd;
        static constexpr uint8_t kSparseMarker = 0xfe;
        static constexpr uint8_t kDoubleNestedArraysMarker = 0xff;

        // Rest is helpers to make these constants easier to use.

        struct TinyNum {
            static constexpr int kMinVal = -10;
            static constexpr int kMaxVal = 31 - 10;  // 21

            static constexpr int kBias = -kMinVal;

            // value is encoded as uint8_t(kTinyTypeZero + value)
            static constexpr uint8_t kTinyIntZero = kTinyIntMin + kBias;
            static constexpr uint8_t kTinyLongZero = kTinyLongMin + kBias;

            static_assert(kTinyIntMin == uint8_t(kTinyIntZero + kMinVal));
            static_assert(kTinyIntMax == uint8_t(kTinyIntZero + kMaxVal));
            static_assert(kTinyLongMin == uint8_t(kTinyLongZero + kMinVal));
            static_assert(kTinyLongMax == uint8_t(kTinyLongZero + kMaxVal));
        };

        struct TinySize {
            static constexpr size_t kStringMax = 64;
            static_assert(kStringSizeMax == kStringSizeMin + TinySize::kStringMax);

            static constexpr size_t kArrInfoMin = 1;         // Never encode 0
            static constexpr size_t kArrInfoMax = 0x1c + 1;  // 29

            // size is encoded as uint8_t(kArrInfoZero + size)
            static constexpr uint8_t kArrInfoZero = kArrInfoSizeTinyMin - kArrInfoMin;
            static_assert(kArrInfoSizeTinyMax == uint8_t(kArrInfoZero + kArrInfoMax));
        };
    };


    ColumnStore(StringData ident) : _ident(std::make_shared<Ident>(ident)) {}
    virtual ~ColumnStore() = default;

    //
    // CRUD
    //
    virtual std::unique_ptr<WriteCursor> newWriteCursor(OperationContext*) = 0;
    virtual void insert(OperationContext*, PathView, RowId, CellView) = 0;
    virtual void remove(OperationContext*, PathView, RowId) = 0;
    virtual void update(OperationContext*, PathView, RowId, CellView) = 0;
    virtual std::unique_ptr<Cursor> newCursor(OperationContext*) const = 0;
    std::unique_ptr<ColumnCursor> newCursor(OperationContext* opCtx, PathView path) const {
        return std::make_unique<ColumnCursor>(path, newCursor(opCtx));
    }

    std::vector<PathValue> uniquePaths(OperationContext* opCtx) const {
        std::vector<PathValue> out;
        PathValue nextPath = "";
        auto cursor = newCursor(opCtx);
        while (auto next = cursor->seekAtOrPast(nextPath, kNullRowId)) {
            out.push_back(next->path.toString());
            nextPath.assign(next->path.rawData(), next->path.size());
            nextPath += '\x01';  // next possible path (\0 is not allowed)
        }
        return out;
    }

    virtual std::unique_ptr<BulkBuilder> makeBulkBuilder(OperationContext* opCtx) = 0;

    //
    // Whole ColumnStore ops
    //
    virtual StatusWith<int64_t> compact(OperationContext* opCtx, const CompactOptions& options) = 0;
    virtual IndexValidateResults validate(OperationContext* opCtx, bool full) const = 0;

    virtual bool appendCustomStats(OperationContext* opCtx,
                                   BSONObjBuilder* output,
                                   double scale) const = 0;

    virtual long long getSpaceUsedBytes(OperationContext* opCtx) const = 0;
    virtual long long getFreeStorageBytes(OperationContext* opCtx) const = 0;

    virtual bool isEmpty(OperationContext* opCtx) = 0;
    virtual int64_t numEntries(OperationContext* opCtx) const = 0;

    /**
     * If the range [*itPtr, end) begins with a number, returns it and positions *itPtr after the
     * last byte of number. If there is no number, returns 0 (which is typically encoded by omitting
     * an optional number) and does not reposition *itPtr.
     */
    static int readArrInfoNumber(StringData::const_iterator* itInOut,
                                 StringData::const_iterator end) {
        auto it = *itInOut;  // Use local to allow compiler to assume it doesn't point to itself.
        size_t res = 0;
        while (it != end && *it >= '0' && *it <= '9') {
            res *= 10;  // noop first pass.
            res += (*it++) - '0';
        }
        *itInOut = it;
        return res;
    }

    static size_t readArrInfoNumber(StringData str, size_t* indexInOut) {
        auto it = str.begin() + *indexInOut;
        auto out = readArrInfoNumber(&it, str.end());
        *indexInOut = it - str.begin();
        return out;
    }

    /**
     * Returns the parent path for the given path, if there is one.
     */
    static boost::optional<PathView> getParentPath(PathView path) {
        auto lastDot = path.rfind('.');
        if (lastDot == std::string::npos) {
            return {};
        }

        return path.substr(0, lastDot);
    }

    std::shared_ptr<Ident> getSharedIdent() const {
        return _ident;
    }

    void setIdent(std::shared_ptr<Ident> newIdent) {
        _ident = std::move(newIdent);
    }

    /**
     * The Cursor class is for raw access to the index. Except for unusual use cases (e.g., index
     * validation) you'll want to use CursorForPath instead.
     */
    class Cursor {
    public:
        virtual ~Cursor() = default;
        virtual boost::optional<FullCellView> next() = 0;
        virtual boost::optional<FullCellView> seekAtOrPast(PathView, RowId) = 0;
        virtual boost::optional<FullCellView> seekExact(PathView, RowId) = 0;

        virtual void save() = 0;
        virtual void saveUnpositioned() {
            save();
        }

        virtual void restore() = 0;

        virtual void detachFromOperationContext() = 0;
        virtual void reattachToOperationContext(OperationContext* opCtx) = 0;

        virtual uint64_t getCheckpointId() const {
            uasserted(ErrorCodes::CommandNotSupported,
                      "The current storage engine does not support checkpoint ids");
        }
    };

    std::shared_ptr<Ident> _ident;
};

/*
 * Array info reader based on a string representation of arrayInfo. Allows for reading/peeking of
 * individual components.
 */
class ArrInfoReader {
public:
    explicit ArrInfoReader(StringData arrInfoStr) : _arrInfo(arrInfoStr) {}

    char nextChar() const {
        if (_offsetInArrInfo == _arrInfo.size()) {
            // Reaching the end of the array info means an unlimited number of '|'s.
            return '|';
        }
        return _arrInfo[_offsetInArrInfo];
    }

    char takeNextChar() {
        if (_offsetInArrInfo == _arrInfo.size()) {
            // Reaching the end of the array info means an unlimited number of '|'s.
            return '|';
        }
        return _arrInfo[_offsetInArrInfo++];
    }

    size_t takeNumber() {
        return ColumnStore::readArrInfoNumber(_arrInfo, &_offsetInArrInfo);
    }

    bool empty() const {
        return _arrInfo.empty();
    }

    /*
     * Returns whether more explicit components are yet to be consumed. Since array info logically
     * ends with an infinite stream of |, this function indicates whether there are more components
     * which are physically present to be read, not including the infinite sequence of |.
     */
    bool moreExplicitComponents() const {
        return _offsetInArrInfo < _arrInfo.size();
    }

    StringData rawArrInfo() const {
        return _arrInfo;
    }

private:
    StringData _arrInfo;
    size_t _offsetInArrInfo = 0;
};

struct SplitCellView {
    StringData arrInfo;  // rawData() is 1-past-end of range starting with firstValuePtr.
    const char* firstValuePtr = nullptr;

    // See column_keygen::UnencodedCellView for a description of each of these flags.
    bool hasDuplicateFields = false;
    bool hasSubPaths = false;
    bool isSparse = false;
    bool hasDoubleNestedArrays = false;

    template <class ValueEncoder>
    struct Cursor {
        using Out = typename std::remove_reference_t<ValueEncoder>::Out;
        Out nextValue() {
            if (elemPtr == end)
                return Out();

            invariant(elemPtr < end);
            return decodeAndAdvance(elemPtr, *encoder);
        }
        bool hasNext() const {
            return elemPtr != end;
        }

        const char* elemPtr;
        const char* end;
        ValueEncoder* encoder;  // Unowned
    };

    /**
     * Construct a cursor that can iterate the values in a column store cell. Requires a
     * 'ValueEncoder' that understands the binary format of cell data.
     *
     * Note: the 'ValueEncoder' is stored as an unowned pointer. The referenced encoder must stay
     * valid for the lifetime of the returned cursor.
     */
    template <class ValueEncoder>
    auto subcellValuesGenerator(ValueEncoder* valEncoder) const {
        return Cursor<ValueEncoder>{firstValuePtr, arrInfo.rawData(), valEncoder};
    }

    template <class ValueEncoder>
    struct CursorWithArrayDepth {
        CursorWithArrayDepth(int pathLength,
                             const char* elemPtr,
                             StringData arrayInfo,
                             ValueEncoder* encoder)
            : elemPtr(elemPtr),
              end(arrayInfo.rawData()),
              encoder(encoder),
              arrInfoReader(arrayInfo),
              pathLength(pathLength) {
            singleSubObject = !hasNext();
        }

        bool hasNext() const {
            return singleSubObject || elemPtr < end || arrInfoReader.moreExplicitComponents();
        }

        using Out = typename std::remove_reference_t<ValueEncoder>::Out;
        struct CellValueWithMetadata {
            Out value{};

            // 'depthWithinDirectlyNestedArraysOnPath' represents nestedness of arrays along the
            // path. That is, only arrays that are elements of other arrays are considered to be
            // nested.
            // Examples (considering path "x.y.z"):
            // 1. {x: {y: {z: 42}}} -- 'depthWithinDirectlyNestedArraysOnPath' of 42 is 0.
            // 2. {x: [{y: [{z: [42]}]}]} -- 'depthWithinDirectlyNestedArraysOnPath' of 42 is 0.
            // 3. {x: {y: {z: [[42]]}}} -- 'depthWithinDirectlyNestedArraysOnPath' of 42 is 0.
            // 4. {x: [[{y: {z: 42}}]]} -- 'depthWithinDirectlyNestedArraysOnPath' of 42 is 1.
            // 5. {x: [[[{y: [[{z: 42}]]}]]]} -- 'depthWithinDirectlyNestedArraysOnPath' of 42 is 3.
            int depthWithinDirectlyNestedArraysOnPath{0};

            // 'depthAtLeaf' represents nestedness of arrays at the leaf of the path, regardless of
            // the array structure along the path. Examples (considering path "x.y.z"):
            // 1. {x: {y: {z: 42}}} -- 'depthAtLeaf' of "42" is 0.
            // 2. {x: [[[[[{y: [[[{z: [42]}]]]}]]]]]} -- 'depthAtLeaf' of "42" is 1.
            int depthAtLeaf{0};

            // When 'isObject' is "true" the 'value' should be ignored. The cursor will return a
            // single "object" value for a range of objects.
            bool isObject{false};
        };
        /**
         * Returns the next value with its array-nestedness level.
         */
        CellValueWithMetadata nextValue() {
            // The expected most common case: implicit tail of values at the same depths.
            if (!arrInfoReader.moreExplicitComponents()) {
                if (singleSubObject) {
                    singleSubObject = false;
                    return {Out{},
                            depthWithinDirectlyNestedArraysOnPath,
                            depthAtLeaf,
                            true /* isObject */};
                }
                return {decodeAndAdvance(elemPtr, *encoder),
                        depthWithinDirectlyNestedArraysOnPath,
                        depthAtLeaf,
                        false};
            }

            // The next expected most common case: a range of values at the same depths.
            if (repeats > 0) {
                repeats--;
                return {decodeAndAdvance(elemPtr, *encoder),
                        depthWithinDirectlyNestedArraysOnPath,
                        depthAtLeaf,
                        false};
            }

            // An end of a range means we have to check for structural changes and update depths.
            while (arrInfoReader.moreExplicitComponents()) {
                switch (arrInfoReader.takeNextChar()) {
                    case '[': {
                        // A '[' can be followed by a number if there are objects in the array,
                        // that should be retrieved from other paths when reconstructing the
                        // record. We can ignore them as they don't contribute to the values.
                        (void)arrInfoReader.takeNumber();

                        if (pathDepth + 1 == pathLength) {
                            depthAtLeaf++;
                        } else if (!inArray.empty() && inArray.top()) {
                            depthWithinDirectlyNestedArraysOnPath++;
                        }
                        inArray.push(true);
                        break;
                    }
                    case '|': {
                        repeats = arrInfoReader.takeNumber();
                        return {decodeAndAdvance(elemPtr, *encoder),
                                depthWithinDirectlyNestedArraysOnPath,
                                depthAtLeaf,
                                false};
                    }
                    case '{': {
                        // We consider as nested only the arrays that are elements of other
                        // arrays. When there is an array of objects and some of the fields of
                        // these objects are arrays, the latter aren't nested.
                        inArray.push(false);
                        pathDepth++;
                        break;
                    }
                    case ']': {
                        invariant(inArray.size() > 0 && inArray.top());
                        inArray.pop();

                        if (pathDepth + 1 == pathLength) {
                            invariant(depthAtLeaf > 0);
                            depthAtLeaf--;
                        } else if (inArray.size() > 0 && inArray.top()) {
                            invariant(depthWithinDirectlyNestedArraysOnPath > 0);
                            depthWithinDirectlyNestedArraysOnPath--;
                        }

                        // Closing an array implicitly closes all objects on the path between it
                        // and the previous array.
                        while (inArray.size() > 0 && !inArray.top()) {
                            inArray.pop();
                            pathDepth--;
                        }
                        break;
                    }
                    case '+': {
                        // Indicates elements in arrays that are objects with sibling paths. These
                        // objects don't contribute to the cell's values, so we can ignore them.
                        // For example, for path "a.b" in
                        // {a: [{b:41}, {c:100}, {c:100}, {b:[{c:100}, 51]}]}
                        //     [  |     +2                {  [o
                        // and the values are 41 and 51
                        (void)arrInfoReader.takeNumber();
                        break;
                    }
                    case 'o': {
                        // Indicates the start of a nested object inside the cell. We don't need
                        // to track this info because the nested objects don't contribute to the
                        // values in the cell.
                        (void)arrInfoReader.takeNumber();
                        return {Out{},
                                depthWithinDirectlyNestedArraysOnPath,
                                depthAtLeaf,
                                true /* isObject */};
                        break;
                    }
                }
            }

            // Start consuming the implicit tail range.
            return {decodeAndAdvance(elemPtr, *encoder),
                    depthWithinDirectlyNestedArraysOnPath,
                    depthAtLeaf,
                    false};
        }

        const char* elemPtr;
        const char* end;
        ValueEncoder* encoder;
        ArrInfoReader arrInfoReader;
        int pathLength = 0;

        int pathDepth = 0;

        // Used to compute the corresponding fields in `CellValueWithMetadata`.
        int depthWithinDirectlyNestedArraysOnPath = 0;
        int depthAtLeaf = 0;

        std::stack<bool, absl::InlinedVector<bool, 64>> inArray;
        size_t repeats = 0;

        // Cells with a single sub-object don't have any values and have empty array info, so we
        // have to track their state separately.
        bool singleSubObject = false;
    };

    static SplitCellView parse(CellView cell) {
        using Bytes = ColumnStore::Bytes;
        using TinySize = ColumnStore::Bytes::TinySize;

        auto out = SplitCellView();
        auto it = cell.data();
        const auto end = cell.data() + cell.size();
        size_t arrInfoSize = 0;

        // This block handles all prefix bytes, and leaves `it` pointing at the first elem.
        // The first two comparisons are technically not needed, but optimize for common cases of no
        // prefix bytes, and an array info size with no other flag bytes.
        if (it != end && uint8_t(*it) >= Bytes::kFirstPrefixByte) {
            if (uint8_t(*it) > Bytes::kLastArrInfoSize) {
                if (it != end && uint8_t(*it) == ColumnStore::Bytes::kDuplicateFieldsMarker) {
                    out.hasDuplicateFields = true;
                    ++it;
                    // This flag is special and should only appear by itself.
                    invariant(it == end);
                    return out;
                }
                if (it != end && uint8_t(*it) == ColumnStore::Bytes::kSubPathsMarker) {
                    out.hasSubPaths = true;
                    ++it;
                }
                if (it != end && uint8_t(*it) == ColumnStore::Bytes::kSparseMarker) {
                    out.isSparse = true;
                    ++it;
                }
                if (it != end && uint8_t(*it) == ColumnStore::Bytes::kDoubleNestedArraysMarker) {
                    out.hasDoubleNestedArrays = true;
                    ++it;
                }

                // Next byte must be either an array info size or a value.
                invariant(it == end || uint8_t(*it) <= Bytes::kLastArrInfoSize);
            }

            if (it != end && Bytes::kFirstArrInfoSize <= uint8_t(*it) &&
                uint8_t(*it) <= Bytes::kLastArrInfoSize) {
                const auto format = uint8_t(*it++);  // Consume size-kind byte.

                if (Bytes::kArrInfoSizeTinyMin <= format && format <= Bytes::kArrInfoSizeTinyMax) {
                    arrInfoSize = format - TinySize::kArrInfoZero;
                } else {
                    switch (format) {
                        case Bytes::kArrInfoSize1:
                            arrInfoSize = ConstDataView(it).read<uint8_t>();
                            it += 1;
                            break;
                        case Bytes::kArrInfoSize2:
                            arrInfoSize = ConstDataView(it).read<LittleEndian<uint16_t>>();
                            it += 2;
                            break;
                        case Bytes::kArrInfoSize4:
                            arrInfoSize = ConstDataView(it).read<LittleEndian<uint32_t>>();
                            it += 4;
                            break;
                        default:
                            MONGO_UNREACHABLE;
                    }
                }
            }
        }

        out.firstValuePtr = it;
        out.arrInfo = cell.substr(cell.size() - arrInfoSize);

        if (it == out.arrInfo.data()) {  // Reminder: beginning of arrInfo is end of values.
            // The lack of any values implies that there must be sub paths.
            out.hasSubPaths = true;
        } else {
            invariant(uint8_t(*it) < Bytes::kFirstPrefixByte);
        }

        return out;
    }

    template <typename Encoder>
    static auto decodeAndAdvance(const char*& ptr, Encoder&& encoder) {
        using Bytes = ColumnStore::Bytes;
        using TinyNum = ColumnStore::Bytes::TinyNum;

        auto byte = uint8_t(*ptr++);

        if (byte >= 0 && byte <= Bytes::kFirstNonBson - 1) {
            --ptr;  // We need the dispatch byte back.
            auto elem = BSONElement(ptr,
                                    1,  // field name size including nul byte
                                    -1  // don't know total element size
            );
            ptr += elem.size();
            return encoder(elem);
        }

        if (byte >= Bytes::kTinyIntMin && byte <= Bytes::kTinyIntMax) {
            return encoder(int32_t(int8_t(byte - TinyNum::kTinyIntZero)));
        } else if (byte >= Bytes::kTinyLongMin && byte <= Bytes::kTinyLongMax) {
            return encoder(int64_t(int8_t(byte - TinyNum::kTinyLongZero)));
        } else if (byte >= Bytes::kStringSizeMin && byte <= Bytes::kStringSizeMax) {
            auto size = size_t(byte - Bytes::kStringSizeMin);
            return encoder(StringData(std::exchange(ptr, ptr + size), size));
        } else {
            switch (byte) {
                    // Whole value encoded in byte.
                case Bytes::kNull:
                    return encoder(BSONNULL);
                case Bytes::kMinKey:
                    return encoder(MINKEY);
                case Bytes::kMaxKey:
                    return encoder(MAXKEY);
                case Bytes::kEmptyObj:
                    return encoder(BSONObj());
                case Bytes::kEmptyArr:
                    return encoder(BSONArray());
                case Bytes::kFalse:
                    return encoder(false);
                case Bytes::kTrue:
                    return encoder(true);
                    // Size and type encoded in byte, value follows.
                case Bytes::kDecimal128: {
                    auto val = encoder(ConstDataView(ptr).read<Decimal128>());
                    ptr += DataType::Handler<Decimal128>::kSizeOfDecimal;
                    return val;
                }
                case Bytes::kDouble: {
                    auto val = ConstDataView(ptr).read<LittleEndian<double>>();
                    ptr += 8;
                    return encoder(double(val));
                }
                case Bytes::kShortDouble: {
                    auto val = ConstDataView(ptr).read<LittleEndian<float>>();
                    ptr += 4;
                    return encoder(double(val));
                }
                case Bytes::kInt1Double: {
                    auto val = ConstDataView(ptr).read<LittleEndian<int8_t>>();
                    ptr += 1;
                    return encoder(double(val));
                }
                case Bytes::kCents1Double: {
                    auto val = ConstDataView(ptr).read<LittleEndian<int8_t>>();
                    ptr += 1;
                    return encoder(double(val) / 100);
                }
                case Bytes::kCents2Double: {
                    auto val = ConstDataView(ptr).read<LittleEndian<int16_t>>();
                    ptr += 2;
                    return encoder(double(val) / 100);
                }
                case Bytes::kCents4Double: {
                    auto val = ConstDataView(ptr).read<LittleEndian<int32_t>>();
                    ptr += 4;
                    return encoder(double(val) / 100);
                }
                case Bytes::kInt1: {
                    auto val = ConstDataView(ptr).read<LittleEndian<int8_t>>();
                    ptr += 1;
                    return encoder(int32_t(val));
                }
                case Bytes::kInt2: {
                    auto val = ConstDataView(ptr).read<LittleEndian<int16_t>>();
                    ptr += 2;
                    return encoder(int32_t(val));
                }
                case Bytes::kInt4: {
                    auto val = ConstDataView(ptr).read<LittleEndian<int32_t>>();
                    ptr += 4;
                    return encoder(int32_t(val));
                }
                case Bytes::kLong1: {
                    auto val = ConstDataView(ptr).read<LittleEndian<int8_t>>();
                    ptr += 1;
                    return encoder(int64_t(val));
                }
                case Bytes::kLong2: {
                    auto val = ConstDataView(ptr).read<LittleEndian<int16_t>>();
                    ptr += 2;
                    return encoder(int64_t(val));
                }
                case Bytes::kLong4: {
                    auto val = ConstDataView(ptr).read<LittleEndian<int32_t>>();
                    ptr += 4;
                    return encoder(int64_t(val));
                }
                case Bytes::kLong8: {
                    auto val = ConstDataView(ptr).read<LittleEndian<int64_t>>();
                    ptr += 8;
                    return encoder(int64_t(val));
                }
                case Bytes::kOID: {
                    auto val = ConstDataView(ptr).read<OID>();
                    ptr += 12;
                    return encoder(val);
                }
                case Bytes::kUUID: {
                    auto val = UUID::fromCDR(ConstDataRange(ptr, 16));
                    ptr += 16;
                    return encoder(val);
                }
                default:
                    MONGO_UNREACHABLE;
            }
        }
    }
};
}  // namespace mongo
