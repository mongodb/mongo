/**
 * Copyright 2011 10gen Inc.
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
#include "FieldIterator.h"

#include "Document.h"

namespace mongo
{
    FieldIterator::FieldIterator(Document *pTheDocument,
				 vector<shared_ptr<Field>> *pTheVFieldPtr):
	pDocument(pTheDocument),
	pVFieldPtr(pTheVFieldPtr),
	currentField(0)
    {
	/* record the number of fields */
	nField = pVFieldPtr->size();
    }

    bool FieldIterator::eof() const
    {
	return currentField >= nField;
    }

    bool FieldIterator::advance()
    {
	assert(!eof());
	++currentField;
	return eof();
    }

    Field *FieldIterator::getCurrent() const
    {
	assert(!eof());
	return (*pVFieldPtr)[currentField].get();
    }
}
