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
*/

#pragma once

namespace mongo {

    namespace dur {

#pragma pack(1)
        /** header for a journal/j._<n> file */
        struct JHeader {
            JHeader() { }
            JHeader(string fname);

            char txt[2];
            unsigned short version;

            // these are just for diagnostic ease
            char n1;
            char ts[20]; 
            char n2;
            char dbpath[128];
            char n3, n4;

            char reserved3[8192 - 68 - 96 + 10 -4]; // 8KB total for the file header
            char txt2[2];

            bool versionOk() const { return version == 0x4141; }
            bool valid() const { return txt[0] == 'j' && txt2[1] == '\n'; }
        };

        /** "Section" header.  A section corresponds to a group commit. */
        struct JSectHeader {
            char txt[4];
            unsigned len; // length in bytes of the whole section
        };

        /** an individual operation within section.  Either the entire section should be applied, or nothing. */
        struct JEntry {
            static const unsigned OpCode_Footer      = 0xffffffff;
            static const unsigned OpCode_DbContext   = 0xfffffffe;
            static const unsigned OpCode_FileCreated = 0xfffffffd;
            static const unsigned OpCode_Min         = 0xfffff000;

            unsigned len; // or sentinel, see structs below
            unsigned ofs; // offset in file
            int fileNo;
            // char data[] follows
        };

        struct JSectFooter { 
            JSectFooter() { 
                sentinel = JEntry::OpCode_Footer;
                hash = 0;
                reserved = 0;
                txt2[0] = txt2[1] = txt2[2] = txt2[3] = '\n';
            }
            unsigned sentinel;
            unsigned hash;
            unsigned long long reserved;
            char txt2[4];
        };

        /** declares "the next entry(s) are for this database / file path prefix" */
        struct JDbContext { 
            JDbContext() : sentinel(JEntry::OpCode_DbContext) { }
            const unsigned sentinel;   // compare to JEntry::len -- zero is our sentinel
            //char dbname[];
        };

#pragma pack()

    }

}
