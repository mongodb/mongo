/**
*    Copyright (C) 2016 MongoDB Inc.
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

#include <memory>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"

namespace mongo {

/**
 * Represents a "view": a virtual collection defined by a query on a collection or another view.
 */
class ViewDefinition {
public:
    /**
     * In the database 'dbName', create a new view 'viewName' on the view or collection
     * 'viewOnName'. Neither 'viewName' nor 'viewOnName' should include the name of the database.
     */
    ViewDefinition(StringData dbName,
                   StringData viewName,
                   StringData viewOnName,
                   const BSONObj& pipeline,
                   std::unique_ptr<CollatorInterface> collation);

    /**
     * Copying a view 'other' clones its collator and does a simple copy of all other fields.
     */
    ViewDefinition(const ViewDefinition& other);
    ViewDefinition& operator=(const ViewDefinition& other);

    /**
     * @return The fully-qualified namespace of this view.
     */
    const NamespaceString& name() const {
        return _viewNss;
    }

    /**
     * @return The fully-qualified namespace of the view or collection upon which this view is
     * based.
     */
    const NamespaceString& viewOn() const {
        return _viewOnNss;
    }

    /**
     * Returns a vector of BSONObjs that represent the stages of the aggregation pipeline that
     * defines this view.
     */
    const std::vector<BSONObj>& pipeline() const {
        return _pipeline;
    }

    /**
     * Returns the default collator for this view.
     */
    const CollatorInterface* defaultCollator() const {
        return _collator.get();
    }

    void setViewOn(const NamespaceString& viewOnNss);

    /**
     * Pipeline must be of type array.
     */
    void setPipeline(const BSONElement& pipeline);

private:
    NamespaceString _viewNss;
    NamespaceString _viewOnNss;
    std::unique_ptr<CollatorInterface> _collator;
    std::vector<BSONObj> _pipeline;
};
}  // namespace mongo
