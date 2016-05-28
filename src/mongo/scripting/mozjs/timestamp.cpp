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

#include "mongo/platform/basic.h"

#include "mongo/scripting/mozjs/timestamp.h"

#include <limits>
#include <string>

#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/internedstring.h"
#include "mongo/scripting/mozjs/objectwrapper.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/valuewriter.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace mozjs {

const char* const TimestampInfo::className = "Timestamp";

namespace {
// Checks that argument 'idx' of 'args' is a number in the range of an unsigned 32-bit integer,
// or uasserts a complaint about an invalid value for 'name'.
double getTimestampArg(JSContext* cx, JS::CallArgs args, int idx, std::string name) {
    int64_t maxArgVal = std::numeric_limits<uint32_t>::max();
    if (!args.get(idx).isNumber())
        uasserted(ErrorCodes::BadValue, str::stream() << name << " must be a number");
    int64_t val = ValueWriter(cx, args.get(idx)).toInt64();
    if (val < 0 || val > maxArgVal) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << name << " must be non-negative and not greater than "
                                << maxArgVal
                                << ", got "
                                << val);
    }
    return val;
}
}  // namespace

void TimestampInfo::construct(JSContext* cx, JS::CallArgs args) {
    auto scope = getScope(cx);

    JS::RootedObject thisv(cx);
    scope->getProto<TimestampInfo>().newObject(&thisv);
    ObjectWrapper o(cx, thisv);

    if (args.length() == 0) {
        o.setNumber(InternedString::t, 0);
        o.setNumber(InternedString::i, 0);
    } else if (args.length() == 2) {
        o.setNumber(InternedString::t, getTimestampArg(cx, args, 0, "Timestamp time (seconds)"));
        o.setNumber(InternedString::i, getTimestampArg(cx, args, 1, "Timestamp increment"));
    } else {
        uasserted(ErrorCodes::BadValue, "Timestamp needs 0 or 2 arguments");
    }

    args.rval().setObjectOrNull(thisv);
}

}  // namespace mozjs
}  // namespace mongo
