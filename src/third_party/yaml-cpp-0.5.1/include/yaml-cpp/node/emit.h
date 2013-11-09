#ifndef NODE_EMIT_H_62B23520_7C8E_11DE_8A39_0800200C9A66
#define NODE_EMIT_H_62B23520_7C8E_11DE_8A39_0800200C9A66

#if defined(_MSC_VER) || (defined(__GNUC__) && (__GNUC__ == 3 && __GNUC_MINOR__ >= 4) || (__GNUC__ >= 4)) // GCC supports "pragma once" correctly since 3.4
#pragma once
#endif

#include <string>
#include <iosfwd>

namespace YAML
{
	class Emitter;
	class Node;
	
	Emitter& operator << (Emitter& out, const Node& node);
	std::ostream& operator << (std::ostream& out, const Node& node);
	
	std::string Dump(const Node& node);
}

#endif // NODE_EMIT_H_62B23520_7C8E_11DE_8A39_0800200C9A66

