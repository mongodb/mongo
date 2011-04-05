/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pch.h"
#include "db/commands/pipeline.h"

#include "db/cursor.h"
#include "db/pipeline/accumulator.h"
#include "db/pipeline/document.h"
#include "db/pipeline/document_source.h"
#include "db/pipeline/expression.h"
#include "db/pdfile.h"

namespace mongo {

    /** mongodb "commands" (sent via db.$cmd.findOne(...))
        subclass to make a command.  define a singleton object for it.
        */
    class PipelineCommand :
        public Command {
    public:
        // virtuals from Command
        virtual ~PipelineCommand();
        virtual bool run(const string& db, BSONObj& cmdObj, string& errmsg,
                         BSONObjBuilder& result, bool fromRepl);
        virtual LockType locktype() const;
        virtual bool slaveOk() const;
        virtual void help(stringstream& help) const;

        PipelineCommand();
    };

    // self-registering singleton static instance
    static PipelineCommand pipelineCommand;

    PipelineCommand::PipelineCommand():
        Command("pipeline") {
    }

    Command::LockType PipelineCommand::locktype() const {
        return READ;
    }

    bool PipelineCommand::slaveOk() const {
        return true;
    }

    void PipelineCommand::help(stringstream &help) const {
        help << "{ pipeline : [ { <data-pipe-op>: {...}}, ... ] }";
    }

    PipelineCommand::~PipelineCommand() {
    }

    bool PipelineCommand::run(const string &db, BSONObj &cmdObj,
                       string &errmsg,
                       BSONObjBuilder &result, bool fromRepl) {
	/* try to parse the command; if this fails, then we didn't run */
	shared_ptr<Pipeline> pPipeline(Pipeline::parseCommand(errmsg, cmdObj));
	if (!pPipeline.get())
	    return false;

	/* now hook up the pipeline */
        /* connect up a cursor to the specified collection */
        shared_ptr<Cursor> pCursor(
            findTableScan(pPipeline->getCollectionName().c_str(), BSONObj()));
        shared_ptr<DocumentSource> pSource(
	    DocumentSourceCursor::create(pCursor));

	return pPipeline->run(result, errmsg, pSource);
    }

} // namespace mongo
