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

#include "pch.h"

#include "Cursor.h"
#include "Document.h"
#include "DocumentSourceCursor.h"
#include "Expression.h"

namespace mongo
{
    DocumentSourceCursor::DocumentSourceCursor(shared_ptr<Cursor> pTheCursor):
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

    shared_ptr<Document> DocumentSourceCursor::getCurrent()
    {
	shared_ptr<Document> pDocument(
	    Document::createFromBsonObj(pCursor->current()));
	return pDocument;
    }

    DocumentSourceCursor::~DocumentSourceCursor()
    {
    }
}
