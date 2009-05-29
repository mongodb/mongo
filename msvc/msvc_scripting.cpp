
#include "stdafx.h"

#if defined(USESM)
#include "..\scripting\engine_spidermonkey.cpp"
#elif defined(NOJNI)
#include "..\scripting\engine_java.cpp"
#else
#include "..\scripting\engine_none.cpp"
#endif