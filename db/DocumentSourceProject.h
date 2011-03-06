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

#include "pch.h"
#include "DocumentSource.h"

namespace mongo
{
    class Document;
    class Expression;

    class DocumentSourceProject :
        public DocumentSource,
	public boost::enable_shared_from_this<DocumentSourceProject>
    {
    public:
	// virtuals from DocumentSource
	virtual ~DocumentSourceProject();
	virtual bool eof();
	virtual bool advance();
	virtual shared_ptr<Document> getCurrent();


	/*
	  Create a new DocumentSource that can implement projection.
	*/
	static shared_ptr<DocumentSourceProject> create(
	    shared_ptr<DocumentSource> pSource);

	/*
	  Add an Expression to the projection.

	  BSON document fields are ordered, so the new field will be 
	  appended to the existing set.

	  @param fieldName the name of the field as it will appear
	  @param pExpression the expression used to compute the field
	*/
	void includeField(string fieldName, shared_ptr<Expression> pExpression);

    private:
	DocumentSourceProject(shared_ptr<DocumentSource> pSource);

	shared_ptr<DocumentSource> pSource; // underlying source
	vector<shared_ptr<Expression>> vpExpression; // inclusions
	vector<string> vFieldName; // inclusion field names
    };
}
