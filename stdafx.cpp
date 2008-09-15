// stdafx.cpp : source file that includes just the standard includes
// db.pch will be the pre-compiled header
// stdafx.obj will contain the pre-compiled type information

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "stdafx.h"

// TODO: reference any additional headers you need in STDAFX.H
// and not in this file

/*
struct MyAsserts {
	MyAsserts() {

	}
} myassertsstdafx;
*/

#undef assert

#undef yassert
#include "assert.h"

void sayDbContext(const char *errmsg = 0);

void wasserted(const char *msg, const char *file, unsigned line) { 
	problem() << "Assertion failure " << msg << ' ' << file << ' ' << line << endl;
	sayDbContext();
}

void asserted(const char *msg, const char *file, unsigned line) {
	wasserted(msg, file, line);
	throw AssertionException();
}

void uasserted(const char *msg) { 
	problem() << "User Assertion " << msg << endl;
	throw UserAssertionException(msg);
}

void msgasserted(const char *msg) {
	log() << "Assertion: " << msg << '\n';
	throw MsgAssertionException(msg);
}
