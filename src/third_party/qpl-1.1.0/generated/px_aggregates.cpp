#include "qplc_api.h"
#include "dispatcher/dispatcher.hpp"
namespace qpl::ml::dispatcher
{
aggregates_table_t px_aggregates_table = {
	px_qplc_bit_aggregates_8u,
	px_qplc_aggregates_8u,
	px_qplc_aggregates_16u,
	px_qplc_aggregates_32u};
}
