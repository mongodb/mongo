#ifndef PTR_VECTOR_H_62B23520_7C8E_11DE_8A39_0800200C9A66
#define PTR_VECTOR_H_62B23520_7C8E_11DE_8A39_0800200C9A66

#if defined(_MSC_VER) || (defined(__GNUC__) && (__GNUC__ == 3 && __GNUC_MINOR__ >= 4) || (__GNUC__ >= 4)) // GCC supports "pragma once" correctly since 3.4
#pragma once
#endif

#include "yaml-cpp/noncopyable.h"
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <vector>

namespace YAML {
	
	template <typename T>
	class ptr_vector: private YAML::noncopyable
	{
	public:
		ptr_vector() {}
		~ptr_vector() { clear(); }
		
		void clear() {
			for(unsigned i=0;i<m_data.size();i++)
				delete m_data[i];
			m_data.clear();
		}
		
		std::size_t size() const { return m_data.size(); }
		bool empty() const { return m_data.empty(); }
		
		void push_back(std::auto_ptr<T> t) {
			m_data.push_back(NULL);
			m_data.back() = t.release();
		}
		T& operator[](std::size_t i) { return *m_data[i]; }
		const T& operator[](std::size_t i) const { return *m_data[i]; }
		
		T& back() { return *m_data.back(); }
		const T& back() const { return *m_data.back(); }

	private:
		std::vector<T*> m_data;
	};
}

#endif // PTR_VECTOR_H_62B23520_7C8E_11DE_8A39_0800200C9A66
