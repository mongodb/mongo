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

#include "Filter.h"
#include "DocumentSourceFilter.h"
#include "Expression.h"

namespace mongo
{
    DocumentSourceFilter::DocumentSourceFilter(
	DocumentSource *pTheSource, Expression *pTheFilter):
	pFilter(pTheFilter)
    {
    }

    bool DocumentSourceFilter::eof()
    {
	// TODO need to look ahead and see
	return pSource->eof();
    }

    bool DocumentSourceFilter::advance()
    {
	return pSource->advance();
    }

    Document *DocumentSourceFilter::getCurrent()
    {
	return NULL; // TODO
    }

    DocumentSourceFilter::~DocumentSourceFilter()
    {
    }
}
