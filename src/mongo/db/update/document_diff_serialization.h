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
const static StringData kArrayHeader = "a"_sd;
const static StringData kDeleteSectionFieldName = "d"_sd;
const static StringData kInsertSectionFieldName = "i"_sd;
const static StringData kUpdateSectionFieldName = "u"_sd;
// 'l' for length.
const static StringData kResizeSectionFieldName = "l"_sd;
const static StringData kSubDiffSectionFieldName = "s"_sd;

// Not meant to be used elsewhere.
class DiffBuilderBase {
public:
    // A child DiffBuilder calls this on its parent when has completed and wants the diff it
    // produced to be stored.
    virtual void completeSubDiff(Diff) = 0;

    // When a child builder is abandoned, it calls this on its parent to indicate that the parent
    // should release any state it was holding for the child.
    virtual void abandonChild() = 0;
};

class DocumentDiffBuilder;

/**
 * Class for building array diffs.
 */
class ArrayDiffBuilder final : public DiffBuilderBase {
public:
    ~ArrayDiffBuilder() {
        if (_parent) {
            BSONObjBuilder builder;
            releaseTo(&builder);
            _parent->completeSubDiff(builder.obj());
        }
    }

    /**
     * Sets the new size of the array. Should only be used for array truncations.
     */
    void setResize(uint32_t index) {
        invariant(!_newSize);
        // BSON only has signed 4 byte integers. The new size must fit into that type.
        invariant(index <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));
        _newSize = static_cast<int32_t>(index);
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

    DocumentDiffBuilder startSubObjDiff(size_t idx);
    ArrayDiffBuilder startSubArrDiff(size_t idx);

    /**
     * Indicates that the changes added to this diff builder should be discarded. After calling
     * this the builder will *not* add anything to its parent builder.
     */
    void abandon() {
        if (_parent) {
            _parent->abandonChild();
            _parent = nullptr;
        }
    }

    void abandonChild() override {
        invariant(_childSubDiffIndex);
        _childSubDiffIndex = {};
    }

    // Called by child builders when they've completed.
    void completeSubDiff(Diff childDiff) override {
        invariant(_childSubDiffIndex);
        _modifications.push_back({*_childSubDiffIndex, std::move(childDiff)});
        _childSubDiffIndex = {};
    }

    size_t computeApproxSize() const {
        // TODO SERVER-48602: We need to ensure that this function returns in O(1). Incrementally
        // computing this size might be one option.
        size_t size = sizeof(int) /* Size of object */ + 1 /* Type byte */ + kArrayHeader.size() +
            1 /* null terminator */ + 1 /*type byte */ + 1 /* bool size*/;

        if (_newSize) {
            size += kResizeSectionFieldName.size() + 1 /* Null terminator */ + 1 /* Type byte */ +
                sizeof(int) /* size of value */;
        }

        for (auto&& [idx, modification] : _modifications) {
            stdx::visit(visit_helper::Overloaded{[idx = idx, &size](const Diff& subDiff) {
                                                     size += sizeof(idx) +
                                                         kSubDiffSectionFieldName.size() +
                                                         subDiff.objsize() + 2;
                                                 },
                                                 [idx = idx, &size](BSONElement elt) {
                                                     size += sizeof(idx) +
                                                         kUpdateSectionFieldName.size() +
                                                         elt.size() + 2;
                                                 }},
                        modification);
        }

        return size;
    }

private:
    // The top-level of a diff is never an array diff. One can only construct an ArrayDiffBuilder
    // from a parent DocumentDiffBuilder.
    explicit ArrayDiffBuilder(DiffBuilderBase* parent) : _parent(parent) {
        invariant(_parent);
    }

    // Dumps the diff into the given builder.
    void releaseTo(BSONObjBuilder* out);

    // Each element is either an update (BSONElement) or a sub diff.
    std::vector<std::pair<size_t, stdx::variant<BSONElement, Diff>>> _modifications;
    boost::optional<int32_t> _newSize;

    // If there is an outstanding child builder, which index it is under.
    boost::optional<size_t> _childSubDiffIndex;

    // Parent DiffBuilder, if any. When this diff builder is destroyed, it will write all of the
    // modifications into its parent (unless it was abandoned).
    DiffBuilderBase* _parent = nullptr;

    friend class DocumentDiffBuilder;
};  // namespace doc_diff

/**
 * Class for building document diffs.
 */
class DocumentDiffBuilder final : public DiffBuilderBase {
public:
    DocumentDiffBuilder() = default;
    ~DocumentDiffBuilder() {
        if (_parent) {
            BSONObjBuilder builder;
            releaseTo(&builder);
            _parent->completeSubDiff(builder.obj());
        }
    }

    /**
     * Produces an owned BSONObj representing the diff.
     */
    Diff release() {
        BSONObjBuilder bob;
        releaseTo(&bob);
        return bob.obj();
    }

    /**
     * Similar to release() but using an existing BSONObjBuilder.
     */
    void releaseTo(BSONObjBuilder* out);

    /**
     * Functions for adding pieces of the diff. These functions must be called no more than once
     * for a given field. They make no attempt to check for duplicates.
     */
    void addDelete(StringData field) {
        _deletes.append(field, false);
    }
    void addUpdate(StringData field, BSONElement value) {
        _updates.appendAs(value, field);
    }
    void addInsert(StringData field, BSONElement value) {
        _inserts.appendAs(value, field);
    }

    /**
     * Methods for starting sub diffs. Must not be called more than once for a given field.  The
     * contents of the StringData passed in must live for the entire duration of the sub-builder's
     * life.
     */
    DocumentDiffBuilder startSubObjDiff(StringData field);
    ArrayDiffBuilder startSubArrDiff(StringData field);

    /**
     * Indicates that the diff being built will never be serialized. If this is a child builder,
     * calling this will ensure that the diff is not added to the parent's buffer on destruction.
     */
    void abandon() {
        if (_parent) {
            _parent->abandonChild();
            _parent = nullptr;
        }
        _deletes.abandon();
        _updates.abandon();
        _inserts.abandon();
    }

    void abandonChild() override {
        invariant(_childSubDiffField);
        _childSubDiffField = {};
    }

    // Called by child builders when they are done.
    void completeSubDiff(Diff childDiff) override {
        invariant(_childSubDiffField);
        _subDiffs.append(*_childSubDiffField, std::move(childDiff));
        _childSubDiffField = {};
    }

    size_t computeApproxSize() {
        // TODO SERVER-48602: We need to ensure that this function returns in O(1). Incrementally
        // computing this size might be one option.
        size_t size = sizeof(int) /* Size of object */ + 1 /* Type byte */;

        if (!_updates.asTempObj().isEmpty()) {
            size += 1 /* Type byte */ + kUpdateSectionFieldName.size() /* FieldName */ +
                1 /* Null terminator */ + _updates.bb().len() + 1 /* Null terminator */;
        }

        if (!_inserts.asTempObj().isEmpty()) {
            size += 1 /* Type byte */ + kInsertSectionFieldName.size() /* FieldName */ +
                1 /* Null terminator */ + _inserts.bb().len() + 1 /* Null terminator */;
        }

        if (!_deletes.asTempObj().isEmpty()) {
            size += 1 /* Type byte */ + kDeleteSectionFieldName.size() /* FieldName */ +
                1 /* Null terminator */ + _deletes.bb().len() + 1 /* Null terminator */;
        }

        if (!_subDiffs.asTempObj().isEmpty()) {
            size += 1 /* Type byte */ + kSubDiffSectionFieldName.size() /* FieldName */ +
                1 /* Null terminator */ + _subDiffs.bb().len() + 1 /* Null terminator */;
        }
        return size;
    }

private:
    DocumentDiffBuilder(DiffBuilderBase* parent) : _parent(parent) {}

    // TODO SERVER-48602: By using a BSONObjBuilder here and again in release() we make two copies
    // of each part of the diff before serializing it. An optimized implementation should avoid
    // this by only creating a BSONObjBuilder when it is time to serialize.
    BSONObjBuilder _subDiffs;
    BSONObjBuilder _deletes;
    BSONObjBuilder _updates;
    BSONObjBuilder _inserts;

    // If there is an outstanding child diff builder, its field is stored here.
    boost::optional<std::string> _childSubDiffField;

    // Only used by sub-builders.
    DiffBuilderBase* _parent = nullptr;

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
