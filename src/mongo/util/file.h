// file.h cross platform basic file class. supports 64 bit offsets and such.

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <boost/cstdint.hpp>
#include <string>

#include "mongo/platform/basic.h"
#include "mongo/platform/cstdint.h"

namespace mongo {

    typedef uint64_t fileofs;

    // NOTE: not thread-safe. (at least the windows implementation isn't)

    class File {

    public:
        File();
        ~File();

        bool bad() const { return _bad; }
        void fsync() const;
        bool is_open() const;
        fileofs len();
        void open(const char* filename, bool readOnly = false, bool direct = false);
        void read(fileofs o, char* data, unsigned len);
        void truncate(fileofs size);
        void write(fileofs o, const char* data, unsigned len);

        static boost::intmax_t freeSpace(const std::string& path);

    private:
        bool _bad;
#ifdef _WIN32
        HANDLE _handle;
#else
        int _fd;
#endif
        std::string _name;

    };

}
