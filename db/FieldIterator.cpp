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
    FieldIterator::FieldIterator(shared_ptr<Document> pTheDocument,
			 const vector<shared_ptr<const Field>> *pTheVFieldPtr):
	pDocument(pTheDocument),
	pVFieldPtr(pTheVFieldPtr),
	currentField(0)
    {
	/* record the number of fields */
	nField = pVFieldPtr->size();
    }

    bool FieldIterator::more() const
    {
	return currentField < nField;
    }

    shared_ptr<const Field> FieldIterator::next()
    {
	assert(more());
	return (*pVFieldPtr)[currentField++];
    }
}
