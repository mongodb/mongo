// @file dur_journalformat.h The format of our journal files.

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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

namespace mongo {

    namespace dur {

        const unsigned Alignment = 8192;

#pragma pack(1)
        /** beginning header for a journal/j._<n> file
            there is nothing important int this header at this time.  except perhaps version #.
        */
        struct JHeader {
            JHeader() { }
            JHeader(string fname);

            char magic[2]; // "j\n". j means journal, then a linefeed, fwiw if you were to run "less" on the file or something...

            // x4142 is asci--readable if you look at the file with head/less -- thus the starting values were near
            // that.  simply incrementing the version # is safe on a fwd basis.
#if defined(_NOCOMPRESS)
            enum { CurrentVersion = 0x4148 };
#else
            enum { CurrentVersion = 0x4149 };
#endif
            unsigned short _version;

            // these are just for diagnostic ease (make header more useful as plain text)
            char n1;          // '\n'
            char ts[20];      // ascii timestamp of file generation.  for user reading, not used by code.
            char n2;          // '\n'
            char dbpath[128]; // path/filename of this file for human reading and diagnostics.  not used by code.
            char n3, n4;      // '\n', '\n'

            unsigned long long fileId; // unique identifier that will be in each JSectHeader. important as we recycle prealloced files

            char reserved3[8026]; // 8KB total for the file header
            char txt2[2];         // "\n\n" at the end

            bool versionOk() const { return _version == CurrentVersion; }
            bool valid() const { return magic[0] == 'j' && txt2[1] == '\n' && fileId; }
        };

        /** "Section" header.  A section corresponds to a group commit.
            len is length of the entire section including header and footer.
            header and footer are not compressed, just the stuff in between.
        */
        struct JSectHeader {
        private:
            unsigned _sectionLen;          // unpadded length in bytes of the whole section
        public:
            unsigned long long seqNumber;  // sequence number that can be used on recovery to not do too much work
            unsigned long long fileId;     // matches JHeader::fileId
            unsigned sectionLen() const { return _sectionLen; }

            // we store the unpadded length so we can use that when we uncompress. to 
            // get the true total size this must be rounded up to the Alignment.
            void setSectionLen(unsigned lenUnpadded) { _sectionLen = lenUnpadded; }

            unsigned sectionLenWithPadding() const { 
                unsigned x = (sectionLen() + (Alignment-1)) & (~(Alignment-1));
                dassert( x % Alignment == 0 );
                return x;
            }
        };

        /** an individual write operation within a group commit section.  Either the entire section should
            be applied, or nothing.  (We check the md5 for the whole section before doing anything on recovery.)
        */
        struct JEntry {
            enum OpCodes {
                OpCode_Footer      = 0xffffffff,
                OpCode_DbContext   = 0xfffffffe,
                OpCode_FileCreated = 0xfffffffd,
                OpCode_DropDb      = 0xfffffffc,
                OpCode_Min         = 0xfffff000
            };
            union {
                unsigned len;    // length in bytes of the data of the JEntry. does not include the JEntry header
                OpCodes opcode;
            };

            unsigned ofs;  // offset in file

            // sentinel and masks for _fileNo
            enum {
                DotNsSuffix = 0x7fffffff, // ".ns" file
                LocalDbBit  = 0x80000000  // assuming "local" db instead of using the JDbContext
            };
            int _fileNo;   // high bit is set to indicate it should be the <dbpath>/local database
            // char data[len] follows

            const char * srcData() const {
                const int *i = &_fileNo;
                return (const char *) (i+1);
            }

            int getFileNo() const { return _fileNo & (~LocalDbBit); }
            void setFileNo(int f) { _fileNo = f; }
            bool isNsSuffix() const { return getFileNo() == DotNsSuffix; }

            void setLocalDbContextBit() { _fileNo |= LocalDbBit; }
            bool isLocalDbContext() const { return _fileNo & LocalDbBit; }
            void clearLocalDbContextBit() { _fileNo = getFileNo(); }

            static string suffix(int fileno) {
                if( fileno == DotNsSuffix ) return "ns";
                stringstream ss;
                ss << fileno;
                return ss.str();
            }
        };

        /** group commit section footer. md5 is a key field. */
        struct JSectFooter {
            JSectFooter();
            JSectFooter(const void* begin, int len); // needs buffer to compute hash
            unsigned sentinel;
            unsigned char hash[16];
            unsigned long long reserved;
            char magic[4]; // "\n\n\n\n"

            /** used by recovery to see if buffer is valid
                @param begin the buffer
                @param len buffer len
                @return true if buffer looks valid
            */
            bool checkHash(const void* begin, int len) const;

            bool magicOk() const { return *((unsigned*)magic) == 0x0a0a0a0a; }
        };

        /** declares "the next entry(s) are for this database / file path prefix" */
        struct JDbContext {
            JDbContext() : sentinel(JEntry::OpCode_DbContext) { }
            const unsigned sentinel;   // compare to JEntry::len -- zero is our sentinel
            //char dbname[];
        };

        /** "last sequence number" */
        struct LSNFile {
            unsigned ver;
            unsigned reserved2;
            unsigned long long lsn;
            unsigned long long checkbytes;
            unsigned long long reserved[8];

            void set(unsigned long long lsn);
            unsigned long long get();
        };

#pragma pack()

    }

}
