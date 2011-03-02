/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "../pch.h"

#include "jsobj.h"
#include "../util/timer.h"

namespace mongo
{
    class BSONObj;
    class BSONObjBuilder;
    class BufBuilder;
    class Client;

    /** mongodb "commands" (sent via db.$cmd.findOne(...))
        subclass to make a command.  define a singleton object for it.
        */
    class Aggregate :
        public Command
    {
    public:
	// virtuals from Command
        virtual bool run(const string& db, BSONObj& cmdObj, string& errmsg,
			 BSONObjBuilder& result, bool fromRepl);
        virtual LockType locktype() const;
        virtual bool slaveOk() const;
        virtual void help( stringstream& help ) const;
        virtual ~Aggregate();

	Aggregate();

    private:
    };

} // namespace mongo
