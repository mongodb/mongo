/**
 *    Copyright (C) 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/bson_serializable.h"

namespace mongo {

/**
 * This class represents the layout and content of a update document runCommand,
 * in the request side.
 */
class BatchedUpdateDocument : public BSONSerializable {
    MONGO_DISALLOW_COPYING(BatchedUpdateDocument);

public:
    //
    // schema declarations
    //

    static const BSONField<BSONObj> query;
    static const BSONField<BSONObj> updateExpr;
    static const BSONField<bool> multi;
    static const BSONField<bool> upsert;
    static const BSONField<BSONObj> collation;

    //
    // construction / destruction
    //

    BatchedUpdateDocument();
    virtual ~BatchedUpdateDocument();

    /** Copies all the fields present in 'this' to 'other'. */
    void cloneTo(BatchedUpdateDocument* other) const;

    //
    // bson serializable interface implementation
    //

    virtual bool isValid(std::string* errMsg) const;
    virtual BSONObj toBSON() const;
    virtual bool parseBSON(const BSONObj& source, std::string* errMsg);
    virtual void clear();
    virtual std::string toString() const;

    //
    // individual field accessors
    //

    void setQuery(const BSONObj& query);
    void unsetQuery();
    bool isQuerySet() const;
    const BSONObj& getQuery() const;

    void setUpdateExpr(const BSONObj& updateExpr);
    void unsetUpdateExpr();
    bool isUpdateExprSet() const;
    const BSONObj& getUpdateExpr() const;

    void setMulti(bool multi);
    void unsetMulti();
    bool isMultiSet() const;
    bool getMulti() const;

    void setUpsert(bool upsert);
    void unsetUpsert();
    bool isUpsertSet() const;
    bool getUpsert() const;

    void setCollation(const BSONObj& collation);
    void unsetCollation();
    bool isCollationSet() const;
    const BSONObj& getCollation() const;

private:
    // Convention: (M)andatory, (O)ptional

    // (M)  query whose result the update will manipulate
    BSONObj _query;
    bool _isQuerySet;

    // (M)  the update expression itself
    BSONObj _updateExpr;
    bool _isUpdateExprSet;

    // (O)  whether multiple documents are to be updated
    bool _multi;
    bool _isMultiSet;

    // (O)  whether upserts are allowed
    bool _upsert;
    bool _isUpsertSet;

    // (O)  the collation which this update should respect.
    BSONObj _collation;
    bool _isCollationSet;
};

}  // namespace mongo
