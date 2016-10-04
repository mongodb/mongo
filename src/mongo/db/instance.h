// instance.h : Global state functions.
//

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/storage_options.h"

namespace mongo {

extern std::string dbExecCommand;

/** a high level recording of operations to the database - sometimes used for diagnostics
    and debugging.
    */
class DiagLog {
    std::ofstream* f;  // note this is never freed
                       /* 0 = off; 1 = writes, 2 = reads, 3 = both
                          7 = log a few reads, and all writes.
                       */
    int level;
    stdx::mutex mutex;
    void openFile();

public:
    DiagLog();
    int getLevel() const {
        return level;
    }
    /**
     * @return old
     */
    int setLevel(int newLevel);
    void flush();
    void writeop(char* data, int len);
    void readop(char* data, int len);
};

extern DiagLog _diaglog;

void assembleResponse(OperationContext* txn,
                      Message& m,
                      DbResponse& dbresponse,
                      const HostAndPort& client);

void maybeCreatePidFile();

}  // namespace mongo
