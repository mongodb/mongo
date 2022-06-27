/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/platform/basic.h"

#include "mongo/util/regex_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/str.h"

namespace mongo {
namespace regex_util {
pcrecpp::RE_Options flagsToPcreOptions(StringData optionFlags, StringData opName) {
    pcrecpp::RE_Options opt;
    opt.set_utf8(true);
    for (auto flag : optionFlags) {
        switch (flag) {
            case 'i':  // case incase sensitive
                opt.set_caseless(true);
                continue;
            case 'm':  // newlines match ^ and $
                opt.set_multiline(true);
                continue;
            case 'x':  // extended mode
                opt.set_extended(true);
                continue;
            case 's':  // allows dot to include newline chars
                opt.set_dotall(true);
                continue;
            case 'u':
                // This option allows Unicode matching for patterns like '\w'. The PCRE library
                // uses this behavior by default, so we don't need to set any options. However, we
                // must accept this flag without an error as some drivers send it by default.
                continue;
            default:
                uasserted(6716200,
                          str::stream() << opName << " invalid flag in regex options: " << flag);
        }
    }
    return opt;
}
}  // namespace regex_util
}  // namespace mongo
