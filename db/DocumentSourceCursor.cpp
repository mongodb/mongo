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

#include "pch.h"

#include "Cursor.h"
#include "DocumentSourceCursor.h"
#include "Expression.h"

namespace mongo
{
    DocumentSourceCursor::DocumentSourceCursor(Cursor *pTheCursor):
	pCursor(pTheCursor)
    {
    }

    bool DocumentSourceCursor::eof()
    {
	return pCursor->eof();
    }

    bool DocumentSourceCursor::advance()
    {
	return pCursor->advance();
    }

    Document *DocumentSourceCursor::getCurrent()
    {
	BSONObj bsonObj(pCursor->current());

	return NULL; // TODO
    }

    DocumentSourceCursor::~DocumentSourceCursor()
    {
    }
}
