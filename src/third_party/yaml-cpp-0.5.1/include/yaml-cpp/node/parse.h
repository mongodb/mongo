#ifndef VALUE_PARSE_H_62B23520_7C8E_11DE_8A39_0800200C9A66
#define VALUE_PARSE_H_62B23520_7C8E_11DE_8A39_0800200C9A66

#if defined(_MSC_VER) || (defined(__GNUC__) && (__GNUC__ == 3 && __GNUC_MINOR__ >= 4) || (__GNUC__ >= 4)) // GCC supports "pragma once" correctly since 3.4
#pragma once
#endif

#include <iosfwd>
#include <string>
#include <vector>

namespace YAML
{
	class Node;
	
	Node Load(const std::string& input);
	Node Load(const char *input);
	Node Load(std::istream& input);
    Node LoadFile(const std::string& filename);

	std::vector<Node> LoadAll(const std::string& input);
	std::vector<Node> LoadAll(const char *input);
	std::vector<Node> LoadAll(std::istream& input);
    std::vector<Node> LoadAllFromFile(const std::string& filename);
}

#endif // VALUE_PARSE_H_62B23520_7C8E_11DE_8A39_0800200C9A66

