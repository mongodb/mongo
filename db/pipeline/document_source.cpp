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

#include "db/pipeline/document_source.h"

namespace mongo {
    DocumentSource::~DocumentSource() {
    }

    void DocumentSource::setSource(shared_ptr<DocumentSource> pTheSource) {
	assert(!pSource.get());
	pSource = pTheSource;
    }

    bool DocumentSource::coalesce(shared_ptr<DocumentSource> pNextSource) {
	return false;
    }

    void DocumentSource::optimize() {
    }

    void DocumentSource::toBson(BSONObjBuilder *pBuilder) const {
	assert(false);
    }
}
