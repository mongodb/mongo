/**
*    Copyright (C) 2011 10gen Inc.
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

#include "pch.h"

namespace mongo
{
    class Document;

    class DocumentSource :
        boost::noncopyable
    {
    public:
	virtual ~DocumentSource() {};

	virtual bool eof() = 0;
        /*
	  @return true if the source has no more Expressions to return.
	*/

	virtual bool advance() = 0;
	/*
	  Advanced the DocumentSource's position in the Document stream.
	*/

	virtual shared_ptr<Document> getCurrent() = 0;
	/*
	  Advance the source, and return the next Expression.

          @return the current Expression
	  TODO throws an exception if there are no more expressions to return.
	*/
    };
}
