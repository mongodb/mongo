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
#include "Value.h"

namespace mongo
{
    class Accumulator;
    class Expression;
    class Value;

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

	  @param fieldName the name the accumulator result will have in the result documents
	  @param pAccumulatorFactory used to create the accumulator for the group field
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
