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

#include "mongo/util/intrusive_counter.h"

#include "mongo/util/mongoutils/str.h"

namespace mongo {
    using namespace mongoutils;

    intrusive_ptr<const RCString> RCString::create(StringData s) {
        const size_t sizeWithNUL = s.size() + 1;
        const size_t bytesNeeded = sizeof(RCString) + sizeWithNUL;
        uassert(16493, str::stream() << "Tried to create string longer than "
                                     << (BSONObjMaxUserSize/1024/1024) << "MB",
                bytesNeeded < static_cast<size_t>(BSONObjMaxUserSize));

        intrusive_ptr<RCString> ptr = new (bytesNeeded) RCString(); // uses custom operator new

        ptr->_size = s.size();
        char* stringStart = reinterpret_cast<char*>(ptr.get()) + sizeof(RCString);
        s.copyTo(stringStart, true);

        return ptr;
    }

    void IntrusiveCounterUnsigned::addRef() const {
        ++counter;
    }

    void IntrusiveCounterUnsigned::release() const {
        if (!--counter)
            delete this;
    }

}
