#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
select_i_table_t px_select_i_table = {
	px_qplc_select_8u_i,
	px_qplc_select_16u_i,
	px_qplc_select_32u_i};
}
