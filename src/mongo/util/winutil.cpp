// winutil.cpp

/*    Copyright 2017 10gen Inc.
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

#if defined(_WIN32)

#include "mongo/platform/basic.h"

#include "mongo/util/winutil.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"

namespace mongo {

StatusWith<boost::optional<DWORD>> windows::getDWORDRegistryKey(const CString& group,
                                                                const CString& key) {
    CRegKey regkey;
    if (ERROR_SUCCESS != regkey.Open(HKEY_LOCAL_MACHINE, group, KEY_READ)) {
        return Status(ErrorCodes::InternalError, "Unable to access windows registry");
    }

    DWORD val;
    const auto res = regkey.QueryDWORDValue(key, val);
    if (ERROR_INVALID_DATA == res) {
        return Status(ErrorCodes::TypeMismatch,
                      "Invalid data type in windows registry, expected DWORD");
    }

    if (ERROR_SUCCESS != res) {
        return boost::none;
    }

    return val;
}

}  // namespace mongo

#endif
