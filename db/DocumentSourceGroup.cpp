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

#include "DocumentSourceGroup.h"

#include "Expression.h"
#include "Field.h"

namespace mongo
{
    DocumentSourceGroup::~DocumentSourceGroup()
    {
    }

    bool DocumentSourceGroup::eof()
    {
	assert(false); // CW TODO unimplemented
	return true;
    }

    bool DocumentSourceGroup::advance()
    {
	assert(false); // CW TODO unimplemented
	return false;
    }

    shared_ptr<Document> DocumentSourceGroup::getCurrent()
    {
	assert(false); // CW TODO unimplemented
	return shared_ptr<Document>();
    }

    shared_ptr<DocumentSourceGroup> DocumentSourceGroup::create(
	shared_ptr<DocumentSource> pTheSource)
    {
	shared_ptr<DocumentSourceGroup> pSource(
	    new DocumentSourceGroup(pTheSource));
	return pSource;
    }

    DocumentSourceGroup::DocumentSourceGroup(
	shared_ptr<DocumentSource> pTheSource):
	pSource(pTheSource)
    {
    }
}
