/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/bson/mutable/document.h"
#include "mongo/db/update/log_builder_interface.h"

namespace mongo {
class RuntimeUpdatePath;

/**
 * LogBuilder abstracts away some of the details of producing a properly constructed oplog $v:1
 * modifier-style update entry. It manages separate regions into which it accumulates $set and
 * $unset operations.
 */
class V1LogBuilder : public LogBuilderInterface {
public:
    /**
     * Construct a new LogBuilder. Log entries will be recorded as new children under the
     * 'logRoot' Element, which must be of type mongo::Object and have no children.
     *
     * The 'includeVersionField' indicates whether the generated log entry should include a $v
     * (version) field.
     */
    V1LogBuilder(mutablebson::Element logRoot, bool includeVersionField = false);

    /**
     * Overloads from LogBuilderInterface. Each of these methods logs a modification to the document
     * in _logRoot. The field name given in the mutablebson element or BSONElement is ignored
     * and the 'path' argument is used instead.
     */
    Status logUpdatedField(const RuntimeUpdatePath& path, mutablebson::Element elt) override;

    /**
     * Logs the creation of a new field. The 'idxOfFirstNewComponent' parameter is unused in this
     * implementation.
     */
    Status logCreatedField(const RuntimeUpdatePath& path,
                           int idxOfFirstNewComponent,
                           mutablebson::Element elt) override;
    Status logCreatedField(const RuntimeUpdatePath& path,
                           int idxOfFirstNewComponent,
                           BSONElement elt) override;

    Status logDeletedField(const RuntimeUpdatePath& path) override;

    /**
     * Return the Document to which the logging root belongs.
     */
    inline mutablebson::Document& getDocument() {
        return _logRoot.getDocument();
    }

    /**
     * Produces a BSON object representing this update using the modifier syntax which can be
     * stored in the oplog.
     */
    BSONObj serialize() const override {
        return _logRoot.getDocument().getObject();
    }

private:
    /**
     * Add the given Element as a new entry in the '$set' section of the log. If a $set section
     * does not yet exist, it will be created. If this LogBuilder is currently configured to
     * contain an object replacement, the request to add to the $set section will return an Error.
     */
    Status addToSets(mutablebson::Element elt);

    /**
     * Convenience method which calls addToSets after
     * creating a new Element to wrap the old one.
     *
     * If any problem occurs then the operation will stop and return that error Status.
     */
    Status addToSetsWithNewFieldName(StringData name, const mutablebson::Element val);

    /**
     * Convenience method which calls addToSets after
     * creating a new Element to wrap the old one.
     *
     * If any problem occurs then the operation will stop and return that error Status.
     */
    Status addToSetsWithNewFieldName(StringData name, const BSONElement& val);

    /**
     * Add the given path as a new entry in the '$unset' section of the log. If an '$unset' section
     * does not yet exist, it will be created. If this LogBuilder is currently configured to
     * contain an object replacement, the request to add to the $unset section will return an
     * Error.
     */
    Status addToUnsets(StringData path);

    Status addToSection(mutablebson::Element newElt,
                        mutablebson::Element* section,
                        const char* sectionName);

    mutablebson::Element _logRoot;
    mutablebson::Element _setAccumulator;
    mutablebson::Element _unsetAccumulator;
};
}  // namespace mongo
