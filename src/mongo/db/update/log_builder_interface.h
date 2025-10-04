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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/exec/mutable_bson/document.h"

namespace mongo {
class RuntimeUpdatePath;

/**
 * Interface for building oplog entries for an update. Provides methods for logging updates,
 * creations and deletes.
 *
 * The caller is expected to make sure all of the paths provided to the log*Field() methods are
 * valid. For example, a sequence of calls which updates field 'a' to the value "foo" and then
 * attempts to update field 'a.b' is a programming error.
 */
class LogBuilderInterface {
public:
    virtual ~LogBuilderInterface() = default;

    /**
     * These methods are used to log a modification to an existing field at given path. The field
     * name provided in the 'elt' argument is ignored.
     */
    virtual Status logUpdatedField(const RuntimeUpdatePath& path, mutablebson::Element elt) = 0;

    /**
     * This method is used to log creation of a new field at the given path. The
     * 'idxOfFirstNewComponent' argument indicates where the _first_ new component in the path
     * is. For example, if an update operating on the document {a: {}} sets field 'a.b.c.d', the
     * first new component would be at index 1 ('b').
     *
     * The field name in the 'elt' argument is ignored.
     */
    virtual Status logCreatedField(const RuntimeUpdatePath& path,
                                   int idxOfFirstNewComponent,
                                   mutablebson::Element elt) = 0;
    virtual Status logCreatedField(const RuntimeUpdatePath& path,
                                   int idxOfFirstNewComponent,
                                   BSONElement elt) = 0;
    /**
     * Logs deletion of a field.
     */
    virtual Status logDeletedField(const RuntimeUpdatePath& path) = 0;

    /**
     * Serializes to a BSONObj which can be put into the 'o' section of an update oplog entry.
     */
    virtual BSONObj serialize() const = 0;
};
}  // namespace mongo
