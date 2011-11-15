// @file logfile.h simple file log writing / journaling

/**
*    Copyright (C) 2010 10gen Inc.
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
*/

#pragma once

namespace mongo {

    class LogFile {
    public:
        /** create the file and open.  must not already exist.
            throws UserAssertion on i/o error
        */
        LogFile(string name, bool readwrite = false);

        /** closes */
        ~LogFile();

        /** append to file.  does not return until sync'd.  uses direct i/o when possible.
            throws UserAssertion on an i/o error
            note direct i/o may have alignment requirements
        */
        void synchronousAppend(const void *buf, size_t len);

        /** write at specified offset. must be aligned.  noreturn until physically written. thread safe */
        void writeAt(unsigned long long offset, const void *_bug, size_t _len);

        void readAt(unsigned long long offset, void *_buf, size_t _len);

        const string _name;

        void truncate(); // Removes extra data after current position

    private:
#if defined(_WIN32)
        typedef HANDLE fd_type;
#else
        typedef int fd_type;
#endif
        fd_type _fd;
        bool _direct; // are we using direct I/O
    };

}
