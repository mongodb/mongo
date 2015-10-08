/*
 *    Copyright (C) 2014 MongoDB Inc.
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
 * This file defines the storage for options that come from the command line related to the
 * mmap v1 storage engine.
 */

namespace mongo {

struct MMAPV1Options {
    MMAPV1Options()
        : lenForNewNsFiles(16 * 1024 * 1024),
          preallocj(true),
          prealloc(false),
          quota(false),
          quotaFiles(8) {}

    // --nssize
    // Specifies the default size for namespace files, which are files that end in .ns.
    // Each collection and index counts as a namespace.
    unsigned lenForNewNsFiles;

    bool preallocj;   // --nopreallocj no preallocation of journal files
    bool prealloc;    // --noprealloc no preallocation of data files
    bool smallfiles;  // --smallfiles allocate smaller data files

    // --journalOptions 7            dump journal and terminate without doing anything further
    // --journalOptions 4            recover and terminate without listening
    enum {                         // bits to be ORed
        JournalDumpJournal = 1,    // dump diagnostics on the journal during recovery
        JournalScanOnly = 2,       // don't do any real work, just scan and dump if dump
                                   // specified
        JournalRecoverOnly = 4,    // terminate after recovery step
        JournalParanoid = 8,       // paranoid mode enables extra checks
        JournalAlwaysCommit = 16,  // do a group commit every time the writelock is released
        JournalAlwaysRemap = 32,   // remap the private view after every group commit
                                   // (may lag to the next write lock acquisition,
                                   // but will do all files then)
        JournalNoCheckSpace = 64   // don't check that there is enough room for journal files
                                   // before startup (for diskfull tests)
    };
    int journalOptions;  // --journalOptions <n> for debugging

    // --quota
    // Enables a maximum limit for the number data files each database can have.
    // When running with the --quota option, MongoDB has a maximum of 8 data files
    // per database. Adjust the quota with --quotaFiles.
    bool quota;

    // --quotaFiles
    // Modifies the limit on the number of data files per database.
    // --quotaFiles option requires that you set --quota.
    int quotaFiles;  // --quotaFiles
};

extern MMAPV1Options mmapv1GlobalOptions;

}  // namespace mongo
