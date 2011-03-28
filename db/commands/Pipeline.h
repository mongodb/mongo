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
    class DocumentSourceProject;
    class Expression;
    class ExpressionNary;
    struct OpDesc; // local private struct

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
	/* these statics should move elsewhere; probably a parser class */
	static shared_ptr<DocumentSource> setupProject(
	    BSONElement *pBsonElement, shared_ptr<DocumentSource> pSource);
	static shared_ptr<DocumentSource> setupFilter(
	    BSONElement *pBsonElement, shared_ptr<DocumentSource> pSource);
	static shared_ptr<DocumentSource> setupGroup(
	    BSONElement *pBsonElement, shared_ptr<DocumentSource> pSource);

	static shared_ptr<Expression> parseOperand(BSONElement *pBsonElement);
	static shared_ptr<Expression> parseExpression(
	    const char *pOpName, BSONElement *pBsonElement);

	class MiniContext
	{
	public:
	    MiniContext(int options);
	    static const int RAVEL_OK = 0x0001;
	    static const int DOCUMENT_OK = 0x0002;

	    bool ravelOk() const;
	    bool ravelUsed() const;
	    void ravel(string fieldName);

	    bool documentOk() const;
	    
	private:
	    int options;
	    string raveledField;
	};

	static shared_ptr<Expression> parseObject(
	    BSONElement *pBsonElement, MiniContext *pCtx);
    };

} // namespace mongo
