// unittest.h

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

#pragma once

/* the idea here is to let all initialization of global variables (classes inheriting from UnitTest)
   complete before we run the tests -- otherwise order of initilization being arbitrary may mess 
   us up.  The app's main() function should call runTests.
*/
struct UnitTest { 
	UnitTest() { registerTest(this); }

	// assert if fails
	virtual void run() = 0;

	static vector<UnitTest*> tests;

	static void registerTest(UnitTest *t) { tests.push_back(t); }

	static void runTests() { 
		for( vector<UnitTest*>::iterator i = tests.begin(); i != tests.end(); i++ ) { 
		}
	}
};

