/* builder.h

*/

#include "../stdafx.h"

class BufBuilder {
public:
	void skip(int n) { }
	char* buf() { return 0; }
	void decouple() { }
	void append(int) { }
	void append(void *, int len) { }
	int len() { return 0; }
};
