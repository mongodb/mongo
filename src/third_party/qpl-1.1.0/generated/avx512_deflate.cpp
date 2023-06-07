#include "deflate_slow_icf.h"
#include "deflate_hash_table.h"
#include "deflate_histogram.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
deflate_table_t avx512_deflate_table = {
	 reinterpret_cast<void *>(&avx512_slow_deflate_icf_body),
	 reinterpret_cast<void *>(&avx512_deflate_histogram_reset),
	 reinterpret_cast<void *>(&avx512_deflate_hash_table_reset)};
}
