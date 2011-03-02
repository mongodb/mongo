/**
 * Copyright (c) 2011 10gen Inc.
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
#include "jsobj.h"

namespace mongo
{
    class Field :
        boost::noncopyable
    {
    public:
	~Field();

	Field(BSONElement bsonElement);

	const char *getName() const;
	/*
	  @return a pointer to the name of the field.
	*/

	BSONType getType() const;
	/*
	  @return the BSON type of the field.
	*/

    private:
	const char *pFieldName;
	BSONType type;
    };
}
