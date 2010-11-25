// @file dur_journal.h

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
    class AlignedBuilder;

    namespace dur {

        /** ok to cleanup journal files at termination?  normally yes, but if any error, no */
        extern bool okToCleanUp;

        /** at termination after db files closed & fsynced */
        void journalCleanup();

        /** assure journal/ dir exists. throws */
        void journalMakeDir();

        /** check if time to rotate files; assure a file is open. 
            done separately from the journal() call as we can do this part
            outside of lock.
         */
        void journalRotate();

        /** write/append to journal */
        void journal(const AlignedBuilder& b);

        /** flag that something has gone wrong during writing to the journal
            (not for recovery mode) 
        */
        void journalingFailure(const char *msg);

#pragma pack(1)
        /** Journal file format stuff */

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
            static const unsigned Sentinel_Footer  = 0xffffffff;
            static const unsigned Sentinel_Context = 0xfffffffe;
            static const unsigned Sentinel_Min     = 0xfffffffe;

            unsigned len; // or sentinel, see structs below
            int fileNo;
            // char data[]
        };

        /** Operations we log that aren't just basic writes.  
         *  Basic writes are logged as JEntry's. 
         *  For each op we define a class.
         */
        /*
        class DurOp : boost::noncopyable { 
        public:
            // @param opcode a sentinel value near max unsigned. 
            DurOp(unsigned opcode);
        };
        
        class PreallocateFileOp : public DurOp { 
        public:
        };*/

        struct JSectFooter { 
            JSectFooter() { 
                sentinel = JEntry::Sentinel_Footer;
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
            JDbContext() : sentinel(JEntry::Sentinel_Context) { }
            const unsigned sentinel;   // compare to JEntry::len -- zero is our sentinel
            //char dbname[];
        };


#pragma pack()

    }
}
