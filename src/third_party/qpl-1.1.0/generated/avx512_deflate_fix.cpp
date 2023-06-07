#include "deflate_slow.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
deflate_fix_table_t avx512_deflate_fix_table = {
	 reinterpret_cast<void *>(&avx512_slow_deflate_body)};
}
