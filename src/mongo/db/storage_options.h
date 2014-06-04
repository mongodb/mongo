/*
 *    Copyright (C) 2013 10gen Inc.
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

#include <string>

/*
 * This file defines the storage for options that come from the command line related to data file
 * persistence.  Many executables that can access data files directly such as mongod and certain
 * tools use these variables, but each executable may have a different set of command line flags
 * that allow the user to change a different subset of these options.
 */

namespace mongo {

    struct StorageGlobalParams {

        StorageGlobalParams() :
#ifdef _WIN32
            dbpath("\\data\\db\\"),
#else
            dbpath("/data/db/"),
#endif
            directoryperdb(false),
            lenForNewNsFiles(16 * 1024 * 1024),
            preallocj(true),
            journalCommitInterval(0), // 0 means use default
            quota(false), quotaFiles(8),
            syncdelay(60),
            useHints(true)
        {
            repairpath = dbpath;
            dur = false;
#if defined(_DURABLEDEFAULTON)
            dur = true;
#endif
            if (sizeof(void*) == 8)
                dur = true;
#if defined(_DURABLEDEFAULTOFF)
            dur = false;
#endif
        }

        std::string dbpath;
        bool directoryperdb;
        std::string repairpath;
        unsigned lenForNewNsFiles;

        bool preallocj;        // --nopreallocj no preallocation of journal files
        bool prealloc;         // --noprealloc no preallocation of data files
        bool smallfiles;       // --smallfiles allocate smaller data files
        bool noTableScan;      // --notablescan no table scans allowed

        bool dur;                       // --dur durability (now --journal)
        unsigned journalCommitInterval; // group/batch commit interval ms

        /** --durOptions 7      dump journal and terminate without doing anything further
            --durOptions 4      recover and terminate without listening
        */
        enum { // bits to be ORed
            DurDumpJournal = 1,   // dump diagnostics on the journal during recovery
            DurScanOnly = 2,      // don't do any real work, just scan and dump if dump specified
            DurRecoverOnly = 4,   // terminate after recovery step
            DurParanoid = 8,      // paranoid mode enables extra checks
            DurAlwaysCommit = 16, // do a group commit every time the writelock is released
            DurAlwaysRemap = 32,  // remap the private view after every group commit (may lag to the
                                  // next write lock acquisition, but will do all files then)
            DurNoCheckSpace = 64  // don't check that there is enough room for journal files before
                                  // startup (for diskfull tests)
        };
        int durOptions;          // --durOptions <n> for debugging

        bool quota;            // --quota
        int quotaFiles;        // --quotaFiles

        double syncdelay;      // seconds between fsyncs

        bool useHints;         // only off if --nohints
    };

    extern StorageGlobalParams storageGlobalParams;

    bool isJournalingEnabled();

    // This is not really related to persistence, but mongos and the other executables share code
    // and we use this function to determine at runtime which executable we are in.
    bool isMongos();

} // namespace mongo
