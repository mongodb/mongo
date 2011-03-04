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
#include "DocumentSource.h"

namespace mongo
{
    class Filter;
    class Expression;

    class DocumentSourceFilter :
        public DocumentSource
    {
    public:
	// virtuals from DocumentSource
	virtual ~DocumentSourceFilter();
	virtual bool eof();
	virtual bool advance();
	virtual shared_ptr<Document> getCurrent();

	DocumentSourceFilter(shared_ptr<DocumentSource> pTheSource,
			     shared_ptr<Expression> pTheFilter);

    private:
	boost::shared_ptr<DocumentSource> pSource;
	boost::shared_ptr<Expression> pFilter;
    };
}
