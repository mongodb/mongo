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
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/ops/modifier_interface.h"
#include "mongo/util/safe_num.h"

namespace mongo {

class LogBuilder;

class ModifierBit : public ModifierInterface {
    MONGO_DISALLOW_COPYING(ModifierBit);

public:
    ModifierBit();
    virtual ~ModifierBit();

    /**
     * A 'modExpr' is a BSONElement {<fieldname>: <value>} coming from a $bit mod such as
     * {$bit: {<field: { [and|or] : <value>}}. init() extracts the field name, the
     * operation subtype, and the value to be assigned to it from 'modExpr'. It returns OK
     * if successful or a status describing the error.
     */
    virtual Status init(const BSONElement& modExpr, const Options& opts, bool* positional = NULL);

    /** Validates the potential application of the init'ed mod to the given Element and
     *  configures the internal state of the mod as necessary.
     */
    virtual Status prepare(mutablebson::Element root, StringData matchedField, ExecInfo* execInfo);

    /** Updates the Element used in prepare with the effects of the $bit operation */
    virtual Status apply() const;

    /** Converts the effects of this $bit into an equivalent $set */
    virtual Status log(LogBuilder* logBuilder) const;

    virtual void setCollator(const CollatorInterface* collator){};

private:
    SafeNum apply(SafeNum value) const;

    // Access to each component of fieldName that's the target of this mod.
    FieldRef _fieldRef;

    // 0 or index for $-positional in _fieldRef.
    size_t _posDollar;

    // The operator on SafeNum that we will invoke.
    typedef SafeNum (SafeNum::*SafeNumOp)(const SafeNum&) const;

    struct OpEntry {
        SafeNum val;
        SafeNumOp op;
    };

    typedef std::vector<OpEntry> OpEntries;

    OpEntries _ops;

    struct PreparedState;
    std::unique_ptr<PreparedState> _preparedState;
};

}  // namespace mongo
