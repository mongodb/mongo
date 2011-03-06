/**
 * Copyright 2011 (c) 10gen Inc.
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

#pragma once

#include "../pch.h"

#include "jsobj.h"
#include "../util/timer.h"
#include "../commands.h"

namespace mongo
{
    class BSONObj;
    class BSONObjBuilder;
    class DocumentSource;

    /** mongodb "commands" (sent via db.$cmd.findOne(...))
        subclass to make a command.  define a singleton object for it.
        */
    class Pipeline :
        public Command
    {
    public:
	// virtuals from Command
        virtual bool run(const string& db, BSONObj& cmdObj, string& errmsg,
			 BSONObjBuilder& result, bool fromRepl);
        virtual LockType locktype() const;
        virtual bool slaveOk() const;
        virtual void help(stringstream& help) const;
        virtual ~Pipeline();

	Pipeline();

    private:
	shared_ptr<DocumentSource> setupProject(
	    BSONElement *pBsonElement, shared_ptr<DocumentSource> pSource);
    };

} // namespace mongo
