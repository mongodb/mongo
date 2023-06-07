#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
select_table_t avx512_select_table = {
	avx512_qplc_select_8u,
	avx512_qplc_select_16u,
	avx512_qplc_select_32u};
}
