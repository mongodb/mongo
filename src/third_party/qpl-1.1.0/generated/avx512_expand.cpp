#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
expand_table_t avx512_expand_table = {
	avx512_qplc_expand_8u,
	avx512_qplc_expand_16u,
	avx512_qplc_expand_32u};
}
