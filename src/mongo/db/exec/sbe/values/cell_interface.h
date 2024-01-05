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

#pragma once

#include <memory>

#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo::sbe::value {
/**
 * CellBlock corresponds to a path for a given document and is a container of values at a "path"
 * according to the chosen definition of "path".
 *
 * For example, a TS bucket has the "data" field that stores the actual data in a columnar format.
 * Each top-level field in the "data" field could be a CellBlock, in which case the "path"
 * definition is the top-level field name.
 */
struct CellBlock {
    /**
     * Limited version of the path language supporting only Get, Traverse, and Id.  For now paths
     * consisting of these operations can be evaluated below the query layer.
     */

    /**
     * Tries to get 'field' from the object and run the remainder of the path on the value at that
     * field.
     */
    struct Get {
        std::string field;
    };

    /**
     * Indicates that an array should be traversed.
     * If the input IS an array, this applies the remainder of the path on every element.
     * If the input IS NOT an array this applies the remainder of the path to the input directly.
     */
    struct Traverse {
        // Nothing
    };

    /**
     * Id component that returns its input (an identity function).
     */
    struct Id {
        // Nothing.
    };

    using Component = std::variant<Get, Traverse, Id>;
    using Path = std::vector<Component>;

    struct PathRequest {
        // The path requested (ie which fields).
        Path path;

        // TODO: May want some other information here, like if we know we can omit certain values
        // etc etc or if we want to specify which type of position info will be needed.

        std::string toString() const;
    };

    virtual ~CellBlock() = default;

    /**
     * Returns the value block for this cell block. The value block is the block of values that
     * corresponds to the path of this cell block.
     */
    virtual ValueBlock& getValueBlock() = 0;

    /**
     * Makes a fully independent clone of this CellBlock.
     */
    virtual std::unique_ptr<CellBlock> clone() const = 0;

    /**
     * Returns an vector of true/false values with a 1 in the position for every new document. E.g.
     * {a: [1,2,3,4]}
     * {a: 5}
     * {XYZ: 999}
     * {a: [6,7]}
     *
     * Values for the 'a' CellBlock:
     * [1,2,3,4,5,Nothing,6,7]
     *
     * Filter position info (the return value of this function):
     * [1,0,0,0,1,1       1,0]
     *
     * A '1' indicates that this value is the beginning of the next document. A '0' indicates this
     * value is part of the same document as the previous entry.
     *
     * An empty vector represents a trivial position info, ie, there are no arrays at all.
     */
    virtual const std::vector<char>& filterPositionInfo() = 0;
};

/*
 * Represents a single path through a block of objects. Stores all of the values found at
 * the given path with eagerly materialized projection and filter position info.
 */
struct MaterializedCellBlock : public CellBlock {
    ValueBlock& getValueBlock() override;
    std::unique_ptr<CellBlock> clone() const override;

    const std::vector<char>& filterPositionInfo() override {
        return _filterPosInfo;
    }

    std::unique_ptr<ValueBlock> _deblocked;
    std::vector<char> _filterPosInfo;
};
}  // namespace mongo::sbe::value
