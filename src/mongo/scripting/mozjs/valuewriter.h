/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <jsapi.h>
#include <string>

#include "mongo/bson/bsonobj.h"
#include "mongo/scripting/mozjs/objectwrapper.h"

namespace mongo {
namespace mozjs {

/**
 * Writes C++ values out of JS Values
 *
 * originalBSON is a hack to keep integer types in their original type when
 * they're read out, manipulated in js and saved back.
 */
class ValueWriter {
public:
    ValueWriter(JSContext* cx, JS::HandleValue value);

    BSONObj toBSON();

    /**
     * These coercions flow through JS::To_X. I.e. they can call toString() or
     * toNumber()
     */
    std::string toString();
    StringData toStringData(JSStringWrapper* jsstr);
    int type();
    double toNumber();
    int32_t toInt32();
    int64_t toInt64();
    Decimal128 toDecimal128();
    bool toBoolean();

    /**
     * Provides the type of the value. For objects, it fetches the class name if possible.
     */
    std::string typeAsString();

    /**
     * Writes the value into a bsonobjbuilder under the name in sd.
     *
     * We take WriteFieldRecursionFrames so we can push a new object on if the underlying
     * value is an object. This allows us to recurse without C++ stack frames.
     *
     * Look in toBSON on ObjectWrapper for the top of that loop.
     */
    void writeThis(BSONObjBuilder* b,
                   StringData sd,
                   ObjectWrapper::WriteFieldRecursionFrames* frames);

    void setOriginalBSON(BSONObj* obj);

private:
    /**
     * Writes the object into a bsonobjbuilder under the name in sd.
     */
    void _writeObject(BSONObjBuilder* b,
                      StringData sd,
                      ObjectWrapper::WriteFieldRecursionFrames* frames);

    JSContext* _context;
    JS::HandleValue _value;
    BSONObj* _originalParent;
};

}  // namespace mozjs
}  // namespace mongo
