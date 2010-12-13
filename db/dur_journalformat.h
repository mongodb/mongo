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

#include "../util/md5.hpp"

namespace mongo {

    namespace dur {

#pragma pack(1)
        /** header for a journal/j._<n> file */
        struct JHeader {
            JHeader() { }
            JHeader(string fname);

            char magic[2]; // "j\n"
            unsigned short version; // 0x4141 or "AA"

            // these are just for diagnostic ease (make header more useful as plain text)
            char n1; // '\n'
            char ts[20]; // offset 6
            char n2; // '\n'
            char dbpath[128]; // offset 27
            char n3, n4; // '\n', '\n'

            char reserved3[8192 - 68 - 96 + 10 -4]; // 8KB total for the file header
            char txt2[2]; // "\n\n" offset 8190

            bool versionOk() const { return version == 0x4141; }
            bool valid() const { return magic[0] == 'j' && txt2[1] == '\n'; }
        };

        /** "Section" header.  A section corresponds to a group commit. */
        struct JSectHeader {
            char magic[4]; // "\nhh\n"
            unsigned len; // length in bytes of the whole section
        };

        /** an individual operation within section.  Either the entire section should be applied, or nothing. */
        struct JEntry {
            enum OpCodes {
                OpCode_Footer      = 0xffffffff,
                OpCode_DbContext   = 0xfffffffe,
                OpCode_FileCreated = 0xfffffffd,
                OpCode_DropDb      = 0xfffffffc,
                OpCode_Min         = 0xfffff000 // higher than max len: OpCode_Min + sizeof(JHeader) > 2^32
            };

            unsigned len; // or opcode, see structs below
            unsigned ofs; // offset in file
            int fileNo;
            // char data[] follows
        };

        struct JSectFooter { 
            JSectFooter(const void* begin, int len) { // needs buffer to compute hash
                sentinel = JEntry::OpCode_Footer;
                reserved = 0;
                magic[0] = magic[1] = magic[2] = magic[3] = '\n';

                // skip section header since size modified after hashing
                (const char*&)begin += sizeof(JSectHeader);
                len                 -= sizeof(JSectHeader);

                md5(begin, len, hash);
            }
            unsigned sentinel;
            md5digest hash; // unsigned char[16]
            unsigned long long reserved;
            char magic[4]; // "\n\n\n\n"

            bool checkHash(const void* begin, int len) const {
                if (*(int*)hash == 0) return true; // TODO(mathias): remove this

                // skip section header since size modified after hashing
                (const char*&)begin += sizeof(JSectHeader);
                len                 -= sizeof(JSectHeader);

                md5digest current;
                md5(begin, len, current);

                return (memcmp(hash, current, sizeof(hash)) == 0);
            }

            // TODO(mathias): remove this
            int size() const {
                if (*(int*)hash == 0)
                    return (sizeof(*this) - sizeof(md5digest) + sizeof(unsigned));
                else
                    return sizeof(*this);
            }
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
