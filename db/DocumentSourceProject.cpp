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
#include "DocumentSourceProject.h"

#include "Document.h"
#include "Expression.h"
#include "Field.h"

namespace mongo
{
    shared_ptr<DocumentSourceProject> DocumentSourceProject::create(
	shared_ptr<DocumentSource> pTheSource)
    {
	shared_ptr<DocumentSourceProject> pSource(
	    new DocumentSourceProject(pTheSource));
	return pSource;
    }

    DocumentSourceProject::DocumentSourceProject(
	shared_ptr<DocumentSource> pTheSource):
	pSource(pTheSource),
	vpExpression()
    {
    }

    bool DocumentSourceProject::eof()
    {
	return pSource->eof();
    }

    bool DocumentSourceProject::advance()
    {
	return pSource->advance();
    }

    shared_ptr<Document> DocumentSourceProject::getCurrent()
    {
	shared_ptr<Document> pInDocument(pSource->getCurrent());
	shared_ptr<Document> pOutDocument(Document::create());

	/* use the expressions to create a new Document out of the old one */
	const size_t n = vpExpression.size();
	for(size_t i = 0; i < n; ++i)
	{
	    shared_ptr<Expression> pExpression(vpExpression[i]);
	    string outName(vFieldName[i]);

	    /* get the value for the field */
	    shared_ptr<Field> pOutField(pExpression->evaluate(pInDocument));

	    /*
	      The name might already be what we want if this is just a
	      pass-through from the underlying source.
	      If the name isn't as requested, rename it.
	    */
	    if (outName.compare(pOutField->getName()) != 0)
		pOutField = Field::createRename(outName, pOutField);

	    /* add the field to the document under construction */
	    pOutDocument->addField(pOutField);
	}

	return pOutDocument;
    }

    DocumentSourceProject::~DocumentSourceProject()
    {
    }

    void DocumentSourceProject::includeField(
	string fieldName, shared_ptr<Expression> pExpression)
    {
	assert(fieldName.length()); // CW TODO must be a non-empty string
	assert(pExpression); // CW TODO must be a non-null expression
	vFieldName.push_back(fieldName);
	vpExpression.push_back(pExpression);
    }
}
