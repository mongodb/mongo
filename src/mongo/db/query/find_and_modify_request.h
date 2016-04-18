/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>

#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/write_concern_options.h"

namespace mongo {

template <typename T>
class StatusWith;

/**
 * Represents the user-supplied options to the findAndModify command. Note that this
 * does not offer round trip preservation. For example, for the case where
 * output = parseBSON(input).toBSON(), 'output' is not guaranteed to be equal to 'input'.
 * However, the semantic meaning of 'output' will be the same with 'input'.
 *
 * The BSONObj members contained within this struct are owned objects.
 */
class FindAndModifyRequest {
public:
    /**
     * Creates a new instance of an 'update' type findAndModify request.
     */
    static FindAndModifyRequest makeUpdate(NamespaceString fullNs,
                                           BSONObj query,
                                           BSONObj updateObj);

    /**
     * Creates a new instance of an 'remove' type findAndModify request.
     */
    static FindAndModifyRequest makeRemove(NamespaceString fullNs, BSONObj query);

    /**
     * Create a new instance of FindAndModifyRequest from a valid BSONObj.
     * Returns an error if the BSONObj is malformed.
     * Format:
     *
     * {
     *   findAndModify: <collection-name>,
     *   query: <document>,
     *   sort: <document>,
     *   collation: <document>,
     *   remove: <boolean>,
     *   update: <document>,
     *   new: <boolean>,
     *   fields: <document>,
     *   upsert: <boolean>
     * }
     *
     * Note: does not parse the writeConcern field or the findAndModify field.
     */
    static StatusWith<FindAndModifyRequest> parseFromBSON(NamespaceString fullNs,
                                                          const BSONObj& cmdObj);

    /**
     * Serializes this object into a BSON representation. Fields that are not
     * set will not be part of the the serialized object.
     */
    BSONObj toBSON() const;

    const NamespaceString& getNamespaceString() const;
    BSONObj getQuery() const;
    BSONObj getFields() const;
    BSONObj getUpdateObj() const;
    BSONObj getSort() const;
    BSONObj getCollation() const;
    bool shouldReturnNew() const;
    bool isUpsert() const;
    bool isRemove() const;

    // Not implemented. Use extractWriteConcern() to get the setting instead.
    WriteConcernOptions getWriteConcern() const;

    //
    // Setters for update type request only.
    //

    /**
     * If shouldReturnNew is new, the findAndModify response should return the document
     * after the modification was applied if the query matched a document. Otherwise,
     * it will return the matched document before the modification.
     */
    void setShouldReturnNew(bool shouldReturnNew);

    void setUpsert(bool upsert);

    //
    // Setters for optional parameters
    //

    /**
     * Specifies the field to project on the matched document.
     */
    void setFieldProjection(BSONObj fields);

    /**
     * Sets the sort order for the query. In cases where the query yields multiple matches,
     * only the first document based on the sort order will be modified/removed.
     */
    void setSort(BSONObj sort);

    /**
     * Sets the collation for the query, which is used for all string comparisons.
     */
    void setCollation(BSONObj collation);

    /**
     * Sets the write concern for this request.
     */
    void setWriteConcern(WriteConcernOptions writeConcern);

private:
    /**
     * Creates a new FindAndModifyRequest with the required fields.
     */
    FindAndModifyRequest(NamespaceString fullNs, BSONObj query, BSONObj updateObj);

    // Required fields
    const NamespaceString _ns;
    const BSONObj _query;

    // Required for updates
    const BSONObj _updateObj;

    boost::optional<bool> _isUpsert;
    boost::optional<BSONObj> _fieldProjection;
    boost::optional<BSONObj> _sort;
    boost::optional<BSONObj> _collation;
    boost::optional<bool> _shouldReturnNew;
    boost::optional<WriteConcernOptions> _writeConcern;

    // Flag used internally to differentiate whether this is an update or remove type request.
    bool _isRemove;
};
}
