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
	vpExpression(),
	ravelField(-1),
	iRavel(0),
	nRavel(0),
	pNoRavelDocument(),
	pRavelField()
    {
    }

    bool DocumentSourceProject::eof()
    {
	/*
	  If we're raveling an array, and there are more elements, then we
	  can return more documents.
	*/
	if ((ravelField >= 0) && (iRavel < nRavel))
	    return false;

	return pSource->eof();
    }

    bool DocumentSourceProject::advance()
    {
	if (ravelField >= 0)
	{
	    if (++iRavel < nRavel)
		return true;

	    /* restart array element iteration for the next document */
	    iRavel = 0;
	}

	/* release the last document and advance */
	pRavelField.reset();
	pNoRavelDocument.reset();
	return pSource->advance();
    }

    shared_ptr<Document> DocumentSourceProject::getCurrent()
    {
	if (!pNoRavelDocument.get())
	{
	    shared_ptr<Document> pInDocument(pSource->getCurrent());
	    pNoRavelDocument = Document::create();

	    /*
	      Use the expressions to create a new Document out of the
	      source Document
	    */
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

		/*
		  If we're raveling this field, and it's an array, then we're
		  going to pick off elements one by one, and make fields of
		  them below.
		*/
		if (((int)i == ravelField) && (pOutField->getType() == Array))
		{
		    pRavelField = pOutField;
		    nRavel = pRavelField->getArray()->size();

		    /*
		      The $ravel of an empty array is a nul value.  If we
		      encounter this, use the non-ravel path, but replace
		      pOutField with a nul.
		    */
		    if (nRavel == 0)
		    {
			pRavelField.reset();
			pOutField = Field::createNul(pOutField->getName());
		    }
		}

		/* add the field to the document under construction */
		pNoRavelDocument->addField(pOutField);
	    }
	}

	/*
	  If we're raveling a field, create an alternate document.  In the
	  alternate (clone), replace the raveled array field with the element
	  at the appropriate index.
	 */
	if (pRavelField.get())
	{
	    /* clone the document with an array we're raveling */
	    shared_ptr<Document> pRavelDocument(
		Document::clone(pNoRavelDocument));

	    /* get access to the array of values */
	    const vector<shared_ptr<Field>> *pvpField = pRavelField->getArray();

	    /* grab the individual array element */
	    shared_ptr<Field> pArrayField((*pvpField)[iRavel]);

	    /* get the output field name */
	    string fieldName(pRavelField->getName());

	    /* make a new Field that has the correct name */
	    shared_ptr<Field> pNamedField(
		Field::createRename(fieldName, pArrayField));

	    /* substitute the named field into the prototype document */
	    pRavelDocument->setField(pNamedField, (size_t)ravelField);

	    return pRavelDocument;
	}

	return pNoRavelDocument;
    }

    DocumentSourceProject::~DocumentSourceProject()
    {
    }

    void DocumentSourceProject::includeField(
	string fieldName, shared_ptr<Expression> pExpression, bool ravelArray)
    {
	assert(fieldName.length()); // CW TODO must be a non-empty string
	assert(pExpression); // CW TODO must be a non-null expression
	assert(!ravelArray || (ravelField < 0));
	    // CW TODO only one ravel allowed

	/* if we're raveling, remember which field */
	if (ravelArray)
	    ravelField = vFieldName.size();

	vFieldName.push_back(fieldName);
	vpExpression.push_back(pExpression);
    }
}
