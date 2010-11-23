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

        /** flag that something has gone wrong */
        void journalingFailure(const char *msg);

#pragma pack(1)
        /** Journal file format stuff */

        /** header for a journal/j._<n> file */
        struct JHeader {
            JHeader() { }
            JHeader(string fname) { 
                txt[0] = 'j'; txt[1] = '\n';
                version = 0x4141;
                memset(ts, 0, sizeof(ts));
                strncpy(ts, time_t_to_String_short(time(0)).c_str(), sizeof(ts)-1);
                memset(dbpath, 0, sizeof(dbpath));
                strncpy(dbpath, fname.c_str(), sizeof(dbpath)-1);
                memset(reserved3, 0, sizeof(reserved3));
                txt2[0] = txt2[1] = '\n';
                n1 = n2 = n3 = n4 = '\n';
            }
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

            unsigned len;
            int fileNo;
            // char data[]
        };

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
