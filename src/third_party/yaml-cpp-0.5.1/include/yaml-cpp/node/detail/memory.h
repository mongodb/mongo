#ifndef VALUE_DETAIL_MEMORY_H_62B23520_7C8E_11DE_8A39_0800200C9A66
#define VALUE_DETAIL_MEMORY_H_62B23520_7C8E_11DE_8A39_0800200C9A66

#if defined(_MSC_VER) || (defined(__GNUC__) && (__GNUC__ == 3 && __GNUC_MINOR__ >= 4) || (__GNUC__ >= 4)) // GCC supports "pragma once" correctly since 3.4
#pragma once
#endif

#include "yaml-cpp/node/ptr.h"
#include <set>
#include <boost/shared_ptr.hpp>

namespace YAML
{
	namespace detail
	{
		class memory {
		public:
			node& create_node();
			void merge(const memory& rhs);
			
		private:
			typedef std::set<shared_node> Nodes;
			Nodes m_nodes;
		};

		class memory_holder {
		public:
			memory_holder(): m_pMemory(new memory) {}
			
			node& create_node() { return m_pMemory->create_node(); }
			void merge(memory_holder& rhs);
			
		private:
			boost::shared_ptr<memory> m_pMemory;
		};
	}
}

#endif // VALUE_DETAIL_MEMORY_H_62B23520_7C8E_11DE_8A39_0800200C9A66
