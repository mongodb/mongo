// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/util/modules.h"

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
