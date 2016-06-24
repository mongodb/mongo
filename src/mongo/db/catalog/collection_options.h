/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <string>

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"

namespace mongo {

struct CollectionOptions {
    CollectionOptions() {
        reset();
    }

    void reset();

    /**
     * Returns true if collection options validates successfully.
     */
    bool isValid() const;

    /**
     * Returns true if the options indicate the namespace is a view.
     */
    bool isView() const;

    /**
     * Confirms that collection options can be converted to BSON and back without errors.
     */
    Status validate() const;

    /**
     * Parses the "options" subfield of the collection info object.
     */
    Status parse(const BSONObj& obj);

    BSONObj toBSON() const;

    /**
     * @param max in and out, will be adjusted
     * @return if the value is valid at all
     */
    static bool validMaxCappedDocs(long long* max);

    // ----

    bool capped;
    long long cappedSize;
    long long cappedMaxDocs;

    // following 2 are mutually exclusive, can only have one set
    long long initialNumExtents;
    std::vector<long long> initialExtentSizes;

    // behavior of _id index creation when collection created
    void setNoIdIndex() {
        autoIndexId = NO;
    }
    enum {
        DEFAULT,  // currently yes for most collections, NO for some system ones
        YES,      // create _id index
        NO        // do not create _id index
    } autoIndexId;

    // user flags
    enum UserFlags {
        Flag_UsePowerOf2Sizes = 1 << 0,
        Flag_NoPadding = 1 << 1,
    };
    int flags;  // a bitvector of UserFlags
    bool flagsSet;

    bool temp;

    // Storage engine collection options. Always owned or empty.
    BSONObj storageEngine;

    // Default options for indexes created on the collection. Always owned or empty.
    BSONObj indexOptionDefaults;

    // Always owned or empty.
    BSONObj validator;
    std::string validationAction;
    std::string validationLevel;

    // The collection's default collation.
    BSONObj collation;

    // View-related options.
    // The namespace of the view or collection that "backs" this view, or the empty string if this
    // collection is not a view.
    std::string viewOn;
    // The aggregation pipeline that defines this view.
    BSONObj pipeline;
};
}
