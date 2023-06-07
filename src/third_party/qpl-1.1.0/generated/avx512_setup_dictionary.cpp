#include "deflate_slow_utils.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
setup_dictionary_table_t avx512_setup_dictionary_table = {
	 reinterpret_cast<void *>(&avx512_setup_dictionary)};
}
