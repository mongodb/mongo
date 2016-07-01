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
#include "mongo/db/ops/modifier_interface.h"

namespace mongo {

class LogBuilder;

class ModifierCurrentDate : public ModifierInterface {
    MONGO_DISALLOW_COPYING(ModifierCurrentDate);

public:
    ModifierCurrentDate();
    virtual ~ModifierCurrentDate();

    /**
     * A 'modExpr' is a BSONElement {<fieldname>: <value>} coming
     * from a $currentDate mod such as
     * {$currentDate: {<fieldname: true/{$type: "date/timestamp"}}.
     * init() extracts the field name and the value to be
     * assigned to it from 'modExpr'. It returns OK if successful or a status describing
     * the error.
     */
    virtual Status init(const BSONElement& modExpr, const Options& opts, bool* positional = NULL);

    /** Evaluates the validity of applying $currentDate.
     */
    virtual Status prepare(mutablebson::Element root, StringData matchedField, ExecInfo* execInfo);

    /** Updates the node passed in prepare with the results from prepare */
    virtual Status apply() const;

    /** Converts the result into a $set */
    virtual Status log(LogBuilder* logBuilder) const;

    virtual void setCollator(const CollatorInterface* collator){};

private:
    // Access to each component of fieldName that's the target of this mod.
    FieldRef _updatePath;

    // 0 or index for $-positional in _updatePath.
    size_t _pathReplacementPosition;

    // Is the final type being added a Date or Timestamp?
    bool _typeIsDate;

    // State which changes with each call of the mod.
    struct PreparedState;
    std::unique_ptr<PreparedState> _preparedState;
};

}  // namespace mongo
