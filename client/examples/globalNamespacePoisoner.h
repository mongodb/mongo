#ifndef STD_CLASHING_TYPES_H
#define STD_CLASHING_TYPES_H

// SERVER-1268
// point out namespace leaks, using 'the usual suspects'
// as representative (if not comprehensive).

// std
struct string {};
struct vector {};
struct list {};
struct set {};
struct map {};
struct multimap {};
struct stack {};
struct queue {};
struct deque {};
struct pair {};
template <typename T1, typename T2>
void make_pair(T1 t1, T2 t2) {}

struct type_info {};

struct iterator {};
struct const_iterator {};
template <typename T>
struct numeric_limits {};

struct stringstream {};
struct ostringstream {};
struct istringstream {};

struct strstream {};

struct ios_base {};
struct hex {};
struct dec {};
struct ostream {};
struct istream {};

struct fstream {};
struct ifstream {};
struct ofstream {};

struct cout {};
struct cin {};
struct cerr {};
struct endl {};

struct exception {};

// boost or C++0x
struct thread {};
struct mutex {};
struct tuple {};
template <typename T>
struct shared_ptr {};

#endif // STD_CLASHING_TYPES_H
