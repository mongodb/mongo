/*
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/db/auth/security_key.h"

#include <string>
#include <sys/stat.h>

#include "mongo/base/status_with.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;

StatusWith<std::string> readSecurityFile(const std::string& filename) {
    struct stat stats;

    // check obvious file errors
    if (stat(filename.c_str(), &stats) == -1) {
        return StatusWith<std::string>(ErrorCodes::InvalidPath,
                                       str::stream() << "Error reading file " << filename << ": "
                                                     << strerror(errno));
    }

#if !defined(_WIN32)
    // check permissions: must be X00, where X is >= 4
    if ((stats.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return StatusWith<std::string>(ErrorCodes::InvalidPath,
                                       str::stream() << "permissions on " << filename
                                                     << " are too open");
    }
#endif

    FILE* file = fopen(filename.c_str(), "rb");
    if (!file) {
        return StatusWith<std::string>(ErrorCodes::InvalidPath,
                                       str::stream() << "error opening file: " << filename << ": "
                                                     << strerror(errno));
    }

    string str = "";

    // strip key file
    const unsigned long long fileLength = stats.st_size;
    unsigned long long read = 0;
    while (read < fileLength) {
        char buf;
        int readLength = fread(&buf, 1, 1, file);
        if (readLength < 1) {
            fclose(file);
            return StatusWith<std::string>(ErrorCodes::UnsupportedFormat,
                                           str::stream() << "error reading file: " << filename);
        }
        read++;

        // check for whitespace
        if ((buf >= '\x09' && buf <= '\x0D') || buf == ' ') {
            continue;
        }

        // check valid base64
        if ((buf < 'A' || buf > 'Z') && (buf < 'a' || buf > 'z') && (buf < '0' || buf > '9') &&
            buf != '+' && buf != '/' && buf != '=') {
            fclose(file);
            return StatusWith<std::string>(
                ErrorCodes::UnsupportedFormat,
                str::stream() << "invalid char in key file " << filename << ": " << buf);
        }

        str += buf;
    }

    fclose(file);
    return StatusWith<std::string>(str);
}

}  // namespace mongo
