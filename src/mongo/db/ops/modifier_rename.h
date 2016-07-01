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

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/ops/modifier_interface.h"

namespace mongo {

class LogBuilder;

/**
* The $rename modifier moves the field from source to the destination to perform
* the rename.
*
* Example: {$rename: {<source>:<dest>}} where both <source/dest> are field names
* Start with {a:1} and applying a {$rename: {"a":"b"} } produces {b:1}
**/
class ModifierRename : public ModifierInterface {
    MONGO_DISALLOW_COPYING(ModifierRename);

public:
    ModifierRename();
    virtual ~ModifierRename();

    /**
     * We will check that the to/from are valid paths; in prepare more validation is done
     */
    virtual Status init(const BSONElement& modExpr, const Options& opts, bool* positional = NULL);

    /**
     * In prepare we will ensure that all restrictions are met:
     *   -- The 'from' field exists, and is valid, else it is a no-op
     *   -- The 'to' field is valid as a destination
     *   -- The 'to' field is not on the path (or the same path) as the 'from' field
     *   -- Neither 'to' nor 'from' have an array ancestor
     */
    virtual Status prepare(mutablebson::Element root, StringData matchedField, ExecInfo* execInfo);

    /**
     * We will transform the document by first making sure that the 'to' element
     * is empty before moving the 'from' element there.
     */
    virtual Status apply() const;

    /**
     * For the oplog entry we will generate an $unset on the 'from' field, and $set for
     * the 'to' field. If no 'from' element is found then function will return immediately.
     */
    virtual Status log(LogBuilder* logBuilder) const;

    virtual void setCollator(const CollatorInterface* collator){};

private:
    // The source and destination fields
    FieldRef _fromFieldRef;
    FieldRef _toFieldRef;

    // The state carried over from prepare for apply/log
    struct PreparedState;
    std::unique_ptr<PreparedState> _preparedState;
};

}  // namespace mongo
