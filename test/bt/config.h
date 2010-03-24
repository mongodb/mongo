/*
 * Configuration for the wts program is an array of string-based paramters.
 * This is the structure used to declare them.
 */
typedef struct {
	const char	*name;			/* Configuration item */

#define	C_FIX		0x01			/* Database types */
#define	C_VAR		0x02
#define	C_ROW		0x04
	u_int8_t	 type_mask;		/* Database type mask */

#define	C_FIXED		0x01			/* Value was set from command-line or file, ignore for this run. */
#define	C_IGNORE	0x02			/* Not a simple randomization, handle outside the main loop. */ 
	u_int8_t	 flags;

	u_int32_t	 min;			/* Minimum value */
	u_int32_t	 max;			/* Maximum value */
	u_int32_t	 *v;			/* Value for this run */
} CONFIG;

/* Get a random value between a config min/max pair. */
#define	CONF_RAND(cp)	MMRAND((cp)->min, (cp)->max)

static CONFIG c[] = {
	{ "bulk_cnt",		0,		0,		0,	M(1),		&g.c_bulk_cnt },
	{ "cache",		0,		0,		1,	30,		&g.c_cache },
	{ "data_max",		0,		0,		32,	4096,		&g.c_data_max },
	{ "data_min",		0,		0,		10,	32,		&g.c_data_min },
	{ "database_type",	0,		C_IGNORE,	1,	3,		&g.c_database_type },
	{ "duplicates",		C_ROW,		C_IGNORE,	0,	1,		&g.c_duplicates },
	{ "duplicates_pct",	C_ROW,		C_IGNORE,	0,	80,		&g.c_duplicates_pct },
	{ "huffman_data",	C_ROW|C_VAR,	0,		0,	1,		&g.c_huffman_data },
	{ "huffman_key",	C_ROW,		0,		0,	1,		&g.c_huffman_key },
	{ "internal_node",	0,		0,		9,	17,		&g.c_internal_node },
	{ "key_cnt",		0,		0,		1000,	M(1),		&g.c_key_cnt },
	{ "key_max",		0,		0,		64,	128,		&g.c_key_max },
	{ "key_min",		0,		0,		10,	32,		&g.c_key_min },
	{ "leaf_node",		0,		0,		9,	17,		&g.c_leaf_node },
	{ "rand_seed",		0,		0,		0,	INT_MAX,	&g.c_rand_seed },
	{ "read_ops",		0,		0,		0,	100,		&g.c_read_ops },
	{ "repeat_comp",	C_FIX,		0,		0,	1,		&g.c_repeat_comp },
	{ "repeat_comp_pct",	C_FIX,		0,		10,	90,		&g.c_repeat_comp_pct },
	{ "runs",		0,		C_IGNORE,	0,	UINT_MAX,	&g.c_runs },
	{ "write_ops",		0,		C_IGNORE,	0,	100,		&g.c_write_ops },
	{ NULL, 0, 0, 0, 0, NULL }
};
