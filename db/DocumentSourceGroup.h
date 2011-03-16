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
    class Expression;

    class DocumentSourceGroup :
        public DocumentSource
    {
    public:
	// virtuals from DocumentSource
	virtual ~DocumentSourceGroup();
	virtual bool eof();
	virtual bool advance();
	virtual shared_ptr<Document> getCurrent();

	static shared_ptr<DocumentSourceGroup> create(
	    shared_ptr<DocumentSource> pSource);

    private:
	DocumentSourceGroup(shared_ptr<DocumentSource> pTheSource);

	shared_ptr<DocumentSource> pSource;
    };
}
