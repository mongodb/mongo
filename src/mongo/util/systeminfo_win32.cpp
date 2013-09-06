/**
 * Copyright (c) 2011 10gen Inc.
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

#include "mongo/pch.h"

#include <windows.h>

#include "mongo/util/systeminfo.h"

namespace mongo {

    size_t SystemInfo::getPhysicalRam() {
        /*
          The value of this should not be changing while the system is running,
          so it should be safe to do this once for the lifetime of the
          application.

          This could present a race condition if multiple threads do this at
          the same time, but all paths through here will produce the same
          result, so it's not worth locking or worrying about it.
         */
        static bool unknown = true;
        static size_t ramSize = 0;

        if (unknown) {
            MEMORYSTATUSEX status;
            status.dwLength = sizeof(status);
            GlobalMemoryStatusEx(&status);
            ramSize = static_cast<size_t>(status.ullTotalPhys);
            unknown = false;
        }

        return ramSize;
    }

}
