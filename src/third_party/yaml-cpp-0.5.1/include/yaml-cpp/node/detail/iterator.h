#ifndef VALUE_DETAIL_ITERATOR_H_62B23520_7C8E_11DE_8A39_0800200C9A66
#define VALUE_DETAIL_ITERATOR_H_62B23520_7C8E_11DE_8A39_0800200C9A66

#if defined(_MSC_VER) || (defined(__GNUC__) && (__GNUC__ == 3 && __GNUC_MINOR__ >= 4) || (__GNUC__ >= 4)) // GCC supports "pragma once" correctly since 3.4
#pragma once
#endif


#include "yaml-cpp/dll.h"
#include "yaml-cpp/node/ptr.h"
#include "yaml-cpp/node/detail/node_iterator.h"
#include <boost/iterator/iterator_adaptor.hpp>
#include <boost/utility.hpp>

namespace YAML
{
	namespace detail
	{
		struct iterator_value;

		template<typename V>
		class iterator_base: public boost::iterator_adaptor<
		iterator_base<V>,
		node_iterator,
		V,
		std::forward_iterator_tag,
		V>
		{
		private:
			template<typename> friend class iterator_base;
			struct enabler {};
			typedef typename iterator_base::base_type base_type;
            
        public:
			typedef typename iterator_base::value_type value_type;
			
		public:
			iterator_base() {}
			explicit iterator_base(base_type rhs, shared_memory_holder pMemory): iterator_base::iterator_adaptor_(rhs), m_pMemory(pMemory) {}
			
			template<class W>
			iterator_base(const iterator_base<W>& rhs, typename boost::enable_if<boost::is_convertible<W*, V*>, enabler>::type = enabler()): iterator_base::iterator_adaptor_(rhs.base()), m_pMemory(rhs.m_pMemory) {}
		
		private:
			friend class boost::iterator_core_access;

			void increment() { this->base_reference() = boost::next(this->base()); }
			
			value_type dereference() const {
				const typename base_type::value_type& v = *this->base();
				if(v.pNode)
					return value_type(Node(*v, m_pMemory));
				if(v.first && v.second)
					return value_type(Node(*v.first, m_pMemory), Node(*v.second, m_pMemory));
				return value_type();
			}
		
		private:
			shared_memory_holder m_pMemory;
		};
	}
}

#endif // VALUE_DETAIL_ITERATOR_H_62B23520_7C8E_11DE_8A39_0800200C9A66
