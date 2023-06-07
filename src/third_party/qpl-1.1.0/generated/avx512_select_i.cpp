#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
select_i_table_t avx512_select_i_table = {
	avx512_qplc_select_8u_i,
	avx512_qplc_select_16u_i,
	avx512_qplc_select_32u_i};
}
