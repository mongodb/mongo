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

#include "db/pipeline/value.h"

namespace mongo
{
    class Accumulator;
    class Cursor;
    class Document;
    class Expression;

    class DocumentSource :
        boost::noncopyable
    {
    public:
	virtual ~DocumentSource() {};

	virtual bool eof() = 0;
        /*
	  @return true if the source has no more Expressions to return.
	*/

	virtual bool advance() = 0;
	/*
	  Advanced the DocumentSource's position in the Document stream.
	*/

	virtual shared_ptr<Document> getCurrent() = 0;
	/*
	  Advance the source, and return the next Expression.

          @return the current Expression
	  TODO throws an exception if there are no more expressions to return.
	*/
    };

    class DocumentSourceCursor :
        public DocumentSource
    {
    public:
	// virtuals from DocumentSource
	virtual ~DocumentSourceCursor();
	virtual bool eof();
	virtual bool advance();
	virtual shared_ptr<Document> getCurrent();

	DocumentSourceCursor(shared_ptr<Cursor> pTheCursor);

    private:
	boost::shared_ptr<Cursor> pCursor;
    };

    class DocumentSourceFilter :
        public DocumentSource
    {
    public:
	// virtuals from DocumentSource
	virtual ~DocumentSourceFilter();
	virtual bool eof();
	virtual bool advance();
	virtual shared_ptr<Document> getCurrent();

	/*
	  Create a filter.

	  @param pTheFilter the expression to use to filter
	  @param pTheSource the underlying source to use
	  @returns the filter
	 */
	static shared_ptr<DocumentSourceFilter> create(
	    shared_ptr<Expression> pTheFilter,
	    shared_ptr<DocumentSource> pTheSource);

    private:
	DocumentSourceFilter(shared_ptr<Expression> pTheFilter,
			     shared_ptr<DocumentSource> pTheSource);

	shared_ptr<DocumentSource> pSource;
	shared_ptr<Expression> pFilter;

	void findNext();

	bool unstarted;
	bool hasNext;
	shared_ptr<Document> pCurrent;
    };

    class DocumentSourceGroup :
        public DocumentSource
    {
    public:
	// virtuals from DocumentSource
	virtual ~DocumentSourceGroup();
	virtual bool eof();
	virtual bool advance();
	virtual shared_ptr<Document> getCurrent();

	/*
	  Create a new grouping DocumentSource.
	 */
	static shared_ptr<DocumentSourceGroup> create(
	    shared_ptr<DocumentSource> pSource);

	/*
	  Set the Id Expression.

	  Documents that pass through the grouping Document are grouped
	  according to this key.  This will generate the id_ field in the
	  result documents.

	  @param pExpression the group key
	 */
	void setIdExpression(shared_ptr<Expression> pExpression);

	/*
	  Add an accumulator.

	  Accumulators become fields in the Documents that result from
	  grouping.  Each unique group document must have it's own
	  accumulator; the accumulator factory is used to create that.

	  @param fieldName the name the accumulator result will have in the
            result documents
	  @param pAccumulatorFactory used to create the accumulator for the
            group field
	 */
	void addAccumulator(string fieldName,
			    shared_ptr<Accumulator> (*pAccumulatorFactory)(),
			    shared_ptr<Expression> pExpression);

    private:
	DocumentSourceGroup(shared_ptr<DocumentSource> pTheSource);

	void populate();
	bool populated;
	shared_ptr<DocumentSource> pSource;

	struct KeyComparator
	{
	    bool operator()(const shared_ptr<const Value> &rL,
			    const shared_ptr<const Value> &rR);
	};

	shared_ptr<Expression> pIdExpression;

	typedef map<shared_ptr<const Value>,
	    vector<shared_ptr<Accumulator>>, KeyComparator> GroupsType;
	GroupsType groups;

	/*
	  The field names for the result documents and the accumulator
	  factories for the result documents.  The Expressions are the
	  common expressions used by each instance of each accumulator
	  in order to find the right-hand side of what gets added to the
	  accumulator.  Note that each of those is the same for each group,
	  so we can share them across all groups by adding them to the
	  accumulators after we use the factories to make a new set of
	  accumulators for each new group.

	  These three vectors parallel each other.
	*/
	vector<string> vFieldName;
	vector<shared_ptr<Accumulator> (*)()> vpAccumulatorFactory;
	vector<shared_ptr<Expression>> vpExpression;


	shared_ptr<Document> makeDocument(const GroupsType::iterator &rIter);

	GroupsType::iterator groupsIterator;
	shared_ptr<Document> pCurrent;

	static string idName; // shared _id string
    };

    class DocumentSourceProject :
        public DocumentSource,
	public boost::enable_shared_from_this<DocumentSourceProject>
    {
    public:
	// virtuals from DocumentSource
	virtual ~DocumentSourceProject();
	virtual bool eof();
	virtual bool advance();
	virtual shared_ptr<Document> getCurrent();


	/*
	  Create a new DocumentSource that can implement projection.
	*/
	static shared_ptr<DocumentSourceProject> create(
	    shared_ptr<DocumentSource> pSource);

	/*
	  Add an Expression to the projection.

	  BSON document fields are ordered, so the new field will be 
	  appended to the existing set.

	  @param fieldName the name of the field as it will appear
	  @param pExpression the expression used to compute the field
	  @param ravelArray if the result of the expression is an array value,
	      the projection will create one Document per value in the array;
	      a sequence of documents is generated, with each one containing one
	      value from the array.  Note there can only be one raveled field.
	*/
	void addField(string fieldName, shared_ptr<Expression> pExpression,
	    bool ravelArray);

    private:
	DocumentSourceProject(shared_ptr<DocumentSource> pSource);

	// configuration state
	shared_ptr<DocumentSource> pSource; // underlying source
	vector<string> vFieldName; // inclusion field names
	vector<shared_ptr<Expression>> vpExpression; // inclusions
	int ravelWhich; // index of raveled field, if any, otherwise -1

	// iteration state
	shared_ptr<Document> pNoRavelDocument; // document to return, pre-ravel
	shared_ptr<const Value> pRavelArray; // field being raveled
	shared_ptr<ValueIterator> pRavel; // iterator used for raveling
	shared_ptr<const Value> pRavelValue; // current value
    };
}


/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo
{
    inline bool DocumentSourceGroup::KeyComparator::operator()(
	const shared_ptr<const Value> &rL,
	const shared_ptr<const Value> &rR)
    {
	return (Value::compare(rL, rR) < 0);
    }

    inline void DocumentSourceGroup::setIdExpression(
	shared_ptr<Expression> pExpression)
    {
	pIdExpression = pExpression;
    }
}
