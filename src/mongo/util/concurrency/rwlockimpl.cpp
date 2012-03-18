// @file rwlockimpl.cpp

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <map>
#include <set>
#include <boost/version.hpp>
#include <boost/noncopyable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition.hpp>

using namespace std;

#include "../assert_util.h"
#include "../time_support.h"
#include "rwlockimpl.h"
