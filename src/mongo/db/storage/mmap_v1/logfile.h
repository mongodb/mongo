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

#pragma once

#include <string>


namespace mongo {

class LogFile {
public:
    /** create the file and open.  must not already exist.
        throws UserAssertion on i/o error
    */
    LogFile(const std::string& name, bool readwrite = false);

    /** closes */
    ~LogFile();

    /** append to file.  does not return until sync'd.  uses direct i/o when possible.
        throws UserAssertion on an i/o error
        note direct i/o may have alignment requirements
    */
    void synchronousAppend(const void* buf, size_t len);

    /** write at specified offset. must be aligned.  noreturn until physically written. thread safe
     * */
    void writeAt(unsigned long long offset, const void* _bug, size_t _len);

    void readAt(unsigned long long offset, void* _buf, size_t _len);

    const std::string _name;

    void truncate();  // Removes extra data after current position

private:
    // Originally disks had a sector size of 512 bytes, after Advanced Format disks were deployed in
    // 2011, the default minimium size became 4096.
    // The direct io size is based on the physical disk sector, not the VM page size.
    const size_t minDirectIOSizeBytes = 4096;

private:
#if defined(_WIN32)
    typedef HANDLE fd_type;
#else
    typedef int fd_type;
#endif
    fd_type _fd;
    bool _direct;  // are we using direct I/O

    // Block size, in case of direct I/O we need to test alignment against the page size,
    // which can be different than 4kB.
    size_t _blkSize;
};
}
