/**
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
 */

#pragma once

#include <string>

#include "mongo/db/commands.h"
#include "mongo/db/commands/write_commands/batch.h"

namespace mongo {

    /**
     * Base class for write commands.  Write commands support batch writes and write concern,
     * and return per-item error information.  All write commands use the (non-virtual) entry
     * point WriteCmd::run().
     *
     * Command parsing is performed by the WriteBatch class (command syntax documented there),
     * and command execution is performed by the WriteBatchExecutor class.
     */
    class WriteCmd : public Command {
        MONGO_DISALLOW_COPYING(WriteCmd);
    public:
        virtual ~WriteCmd() {}

    protected:
        /**
         * Instantiates a command that can be invoked by "name", which will be capable of issuing
         * write batches of type "writeType", and will require privilege "action" to run.
         */
        WriteCmd(const StringData& name, WriteBatch::WriteType writeType, ActionType action);

    private:
        virtual bool logTheOp();

        virtual bool slaveOk() const;

        virtual LockType locktype() const;

        virtual void addRequiredPrivileges(const std::string& dbname,
                                   const BSONObj& cmdObj,
                                   std::vector<Privilege>* out);

        virtual bool shouldAffectCommandCounter() const;

        // Write command entry point.
        virtual bool run(const string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl);

        // Privilege required to execute command.
        ActionType _action;

        // Type of batch (e.g. insert).
        WriteBatch::WriteType _writeType;
    };

    class CmdInsert : public WriteCmd {
        MONGO_DISALLOW_COPYING(CmdInsert);
    public:
        CmdInsert();

    private:
        virtual void help(stringstream& help) const;
    };

    class CmdUpdate : public WriteCmd {
        MONGO_DISALLOW_COPYING(CmdUpdate);
    public:
        CmdUpdate();

    private:
        virtual void help(stringstream& help) const;
    };

    class CmdDelete : public WriteCmd {
        MONGO_DISALLOW_COPYING(CmdDelete);
    public:
        CmdDelete();

    private:
        virtual void help(stringstream& help) const;
    };

} // namespace mongo
