/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/visit_helper.h"

// This file contains classes for serializing document diffs to a format that can be stored in the
// oplog. Any code/machinery which manipulates document diffs should do so through these classes.

namespace mongo {
namespace doc_diff {
using Diff = BSONObj;

/**
 * Enum representing the type of a diff.
 */
enum DiffType : uint8_t { kDocument, kArray };

// Below are string constants used in the diff format.
constexpr StringData kArrayHeader = "a"_sd;
constexpr StringData kDeleteSectionFieldName = "d"_sd;
constexpr StringData kInsertSectionFieldName = "i"_sd;
constexpr StringData kUpdateSectionFieldName = "u"_sd;
// 'l' for length.
constexpr StringData kResizeSectionFieldName = "l"_sd;
constexpr StringData kSubDiffSectionFieldName = "s"_sd;

// Below are constants used for computation of Diff size. Note that the computed size is supposed to
// be an approximate value.
constexpr size_t kAdditionalPaddingForObjectSize = 5;
constexpr size_t kSizeOfEmptyDocumentDiffBuilder = 5;
// Size of empty object(5) + kArrayHeader(1) + null terminator + type byte + bool size.
constexpr size_t kSizeOfEmptyArrayDiffBuilder = 9;

/**
 * Internal helper class for BSON size tracking to be used by the DiffBuilder classes.
 */
struct ApproxBSONSizeTracker {
    ApproxBSONSizeTracker(size_t initialSize) : _size(initialSize) {}

    void addEntry(size_t fieldSize, size_t valueSize, bool needToCreateWrappingObject = false) {
        if (needToCreateWrappingObject) {
            // Type byte(1) + FieldName(1) + Null terminator(1) + empty BSON object size (5)
            _size += 8;
        }
        _size += fieldSize + valueSize + 2 /* Type byte + null terminator for field name */;
    }

    size_t getSize() const {
        return _size;
    }

private:
    size_t _size;
};

template <class DiffBuilder>
class SubBuilderGuard;
class DocumentDiffBuilder;
class ArrayDiffBuilder;

// Not meant to be used elsewhere.
class DiffBuilderBase {
public:
    DiffBuilderBase(size_t size) : sizeTracker(size){};
    virtual ~DiffBuilderBase(){};

    // Serializes the diff to 'out'.
    virtual void serializeTo(BSONObjBuilder* out) const = 0;
    size_t getObjSize() const {
        return sizeTracker.getSize();
    }

protected:
    ApproxBSONSizeTracker sizeTracker;

private:
    // When the current child builder is abandoned, we needs to call this to indicate that the
    // DiffBuilder should release any state it was holding for the current child. After calling
    // this the builder will *not* add anything to the child builder.
    virtual void abandonChild() = 0;

    // When the current child builder is finished, we needs to call this to indicate that the
    // DiffBuilder should stop accepting further modifications to the current child. After calling
    // this the builder will *not* add anything to the child builder.
    virtual void finishChild() = 0;

    friend class SubBuilderGuard<ArrayDiffBuilder>;
    friend class SubBuilderGuard<DocumentDiffBuilder>;
};

/**
 * An RAII type class which provides access to a sub-diff builder. This can be used to let the
 * parent builder know what to do with the current builder resource, i.e, either commit or destroy.
 * Note that this class is not directly responsible for destroying the diff builders.
 */
template <class DiffBuilder>
class SubBuilderGuard final {
public:
    SubBuilderGuard(DiffBuilderBase* parent, DiffBuilder* builder)
        : _parent(parent), _builder(builder) {}
    ~SubBuilderGuard() {
        if (_parent) {
            _parent->finishChild();
        }
    }

    void abandon() {
        _parent->abandonChild();
        _parent = nullptr;
        _builder = nullptr;
    }

    DiffBuilder* builder() {
        return _builder;
    }

private:
    // Both pointers are unowned.
    DiffBuilderBase* _parent;
    DiffBuilder* _builder;
};

/**
 * Class for building array diffs. The diff builder does not take ownership of the BSONElement
 * passed. It is the responsibility of the caller to ensure that data stays in scope until
 * serializeTo() is called.
 */
class ArrayDiffBuilder final : public DiffBuilderBase {
public:
    using ArrayModification = stdx::variant<BSONElement, std::unique_ptr<DiffBuilderBase>>;
    ~ArrayDiffBuilder() {}

    /**
     * Sets the new size of the array. Should only be used for array truncations.
     */
    void setResize(uint32_t index) {
        invariant(!_newSize);
        // BSON only has signed 4 byte integers. The new size must fit into that type.
        invariant(index <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));
        _newSize = static_cast<int32_t>(index);
        sizeTracker.addEntry(kResizeSectionFieldName.size(), sizeof(uint32_t) /* size of value */);
    }

    /*
     * Below are functions for adding pieces of the diff. These functions must be called no more
     * than once for a given array index. They make no attempt to check for duplicates.
     */

    /**
     * Adds an array index to be updated with a new value. Must be called with ascending values for
     * 'idx'. For example, you cannot call this with idx = 5 and then call it again with idx = 4.
     */
    void addUpdate(size_t idx, BSONElement newValue);

    /**
     * Starts a sub-diff object and returns an unowned pointer to the sub-diff builder. The client
     * should not try to destroy the object.
     */
    SubBuilderGuard<DocumentDiffBuilder> startSubObjDiff(size_t idx);
    SubBuilderGuard<ArrayDiffBuilder> startSubArrDiff(size_t idx);

    // Dumps the diff into the given builder.
    void serializeTo(BSONObjBuilder* out) const;

private:
    // The top-level of a diff is never an array diff. One can only construct an ArrayDiffBuilder
    // from a parent DocumentDiffBuilder.
    explicit ArrayDiffBuilder() : DiffBuilderBase(kSizeOfEmptyArrayDiffBuilder) {}

    void abandonChild() override {
        invariant(!_modifications.empty());
        _modifications.pop_back();
    }

    void finishChild() override {
        invariant(!_modifications.empty());
        sizeTracker.addEntry(
            _modifications.back().first.size() + kSubDiffSectionFieldName.size(),
            stdx::get<std::unique_ptr<DiffBuilderBase>>(_modifications.back().second)
                ->getObjSize());
    }

    // Each element is either an update (BSONElement) or a sub diff.
    std::vector<std::pair<std::string, ArrayModification>> _modifications;

    boost::optional<int32_t> _newSize;

    friend class DocumentDiffBuilder;
};  // namespace doc_diff

/**
 * Class for building document diffs. The diff builder does not take ownership of either the field
 * name or the BSONElement passed. It is the responsibility of the caller to ensure that data stays
 * in scope until serialize() is called.
 */
class DocumentDiffBuilder final : public DiffBuilderBase {
public:
    DocumentDiffBuilder(size_t padding = kAdditionalPaddingForObjectSize)
        : DiffBuilderBase(kSizeOfEmptyDocumentDiffBuilder + padding) {}

    /**
     * Produces an owned BSONObj representing the diff.
     */
    Diff serialize() const {
        BSONObjBuilder bob;
        serializeTo(&bob);
        return bob.obj();
    }

    /**
     * Similar to serialize() but using an existing BSONObjBuilder.
     */
    void serializeTo(BSONObjBuilder* out) const;

    /**
     * Functions for adding pieces of the diff. These functions must be called no more than once
     * for a given field. They make no attempt to check for duplicates.
     */
    void addDelete(StringData field) {
        // Add the size of 'field' + 'value'.
        sizeTracker.addEntry(field.size(), sizeof(char), _deletes.empty());

        _deletes.push_back(field);
    }
    void addUpdate(StringData field, BSONElement value) {
        // Add the size of 'field' + 'value'.
        sizeTracker.addEntry(field.size(), value.valuesize(), _updates.empty());

        _updates.push_back({field, value});
    }
    void addInsert(StringData field, BSONElement value) {
        // Add the size of 'field' + 'value'.
        sizeTracker.addEntry(field.size(), value.valuesize(), _inserts.empty());

        _inserts.push_back({field, value});
    }

    /**
     * Methods for starting sub diffs. Must not be called more than once for a given field.  The
     * contents of the StringData passed in must live for the entire duration of the sub-builder's
     * life. Returns an unowned pointer to the sub-diff builder. The client should not try to
     * destroy the object.
     */
    SubBuilderGuard<DocumentDiffBuilder> startSubObjDiff(StringData field);
    SubBuilderGuard<ArrayDiffBuilder> startSubArrDiff(StringData field);

private:
    void abandonChild() override {
        invariant(!_subDiffs.empty());
        _subDiffs.pop_back();
    }

    void finishChild() override {
        invariant(!_subDiffs.empty());

        // Add the size of 'field' + 'value'.
        sizeTracker.addEntry(_subDiffs.back().first.size(),
                             _subDiffs.back().second->getObjSize(),
                             _subDiffs.size() == 1);
    }
    std::vector<std::pair<StringData, BSONElement>> _updates;
    std::vector<std::pair<StringData, BSONElement>> _inserts;
    std::vector<StringData> _deletes;
    std::vector<std::pair<StringData, std::unique_ptr<DiffBuilderBase>>> _subDiffs;

    // If there is an outstanding child diff builder, its field is stored here.
    boost::optional<std::string> _childSubDiffField;

    friend class ArrayDiffBuilder;
};

/*
 * The below classes are for reading diffs. When given an invalid/malformed diff they will uassert.
 */
class DocumentDiffReader;

class ArrayDiffReader {
public:
    using ArrayModification = stdx::variant<BSONElement, DocumentDiffReader, ArrayDiffReader>;

    explicit ArrayDiffReader(const Diff& diff);

    /**
     * Methods which return the next modification (if any) and advance the iterator.  Returns
     * boost::none if there are no remaining modifications.  Otherwise, returns a pair. The first
     * member of the pair is the index of the modification. The second part is the modification
     * itself.
     *
     * If the second part of the pair contains a BSONElement, it means there is an update at that
     * index.
     *
     * If the second part contains a DocumentDiffReader or ArrayDiffReader, it means there is a
     * subdiff at that index.
     */
    boost::optional<std::pair<size_t, ArrayModification>> next();

    /**
     * Returns the size of the post image array.
     */
    boost::optional<size_t> newSize() {
        return _newSize;
    }

private:
    Diff _diff;
    BSONObjIterator _it;

    boost::optional<size_t> _newSize;
};

class DocumentDiffReader {
public:
    DocumentDiffReader(const Diff& diff);

    /**
     * The below methods get the next type of modification (if any) and advance the iterator.
     */
    boost::optional<StringData> nextDelete();
    boost::optional<BSONElement> nextUpdate();
    boost::optional<BSONElement> nextInsert();
    boost::optional<std::pair<StringData, stdx::variant<DocumentDiffReader, ArrayDiffReader>>>
    nextSubDiff();

private:
    Diff _diff;

    boost::optional<BSONObjIterator> _deletes;
    boost::optional<BSONObjIterator> _inserts;
    boost::optional<BSONObjIterator> _updates;
    boost::optional<BSONObjIterator> _subDiffs;
};
}  // namespace doc_diff
}  // namespace mongo
