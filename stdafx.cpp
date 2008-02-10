// stdafx.cpp : source file that includes just the standard includes
// db.pch will be the pre-compiled header
// stdafx.obj will contain the pre-compiled type information

#include "stdafx.h"

// TODO: reference any additional headers you need in STDAFX.H
// and not in this file

struct MyAsserts {
	MyAsserts() {

	}
} myassertsstdafx;

#undef assert

#undef yassert
#include "assert.h"

void sayDbContext();

void asserted(const char *msg, const char *file, unsigned line) { 
	cout << "Assertion failure " << msg << endl;
	cout << ' ' << file << ' ' << line << endl;
	sayDbContext();
	throw AssertionException();
}
