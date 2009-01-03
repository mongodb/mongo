#include "stdafx.h"
#include <stdio.h>
//#include <iostream>
#include "../util/goodies.h"

//using namespace std;

#undef cout
#undef endl

int main(int argc, char* argv[], char *envp[] ) {
	cout << "hello" << endl;

	FILE *f = fopen("/data/db/temptest", "a");

	if( f == 0 ) {
		cout << "can't open file\n";
		return 1;
	}

	{
		Timer t;
		for( int i = 0; i < 50000; i++ )
			fwrite("abc", 3, 1, f);
		cout << "small writes: " << t.millis() << "ms" << endl;
	}
    
	{
		Timer t;
		for( int i = 0; i < 50000; i++ ) {
			fwrite("abc", 3, 1, f);
			fflush(f);
            fsync( fileno( f ) );
		}
		cout << "flush: " << t.millis() << "ms" << endl;
	}

	{
		Timer t;
		for( int i = 0; i < 500; i++ ) {
			fwrite("abc", 3, 1, f);
			fflush(f);
            fsync( fileno( f ) );
			sleepmillis(10);
		}
		cout << "flush with 5000 sleep: " << t.millis() << "ms" << endl;
	}
    
	return 0;
}
