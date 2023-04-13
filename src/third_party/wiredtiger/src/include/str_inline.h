/*
 * __wt_prepare_state_str --
 *     Convert a prepare state to its string representation.
 */
static inline const char *
__wt_prepare_state_str(uint8_t val)
{
    switch (val) {
    case WT_PREPARE_INIT:
        return ("WT_PREPARE_INIT");
    case WT_PREPARE_INPROGRESS:
        return ("WT_PREPARE_INPROGRESS");
    case WT_PREPARE_LOCKED:
        return ("WT_PREPARE_LOCKED");
    case WT_PREPARE_RESOLVED:
        return ("WT_PREPARE_RESOLVED");
    }

    return ("PREPARE_STATE_INVALID");
}

/*
 * __wt_update_type_str --
 *     Convert an update type to its string representation.
 */
static inline const char *
__wt_update_type_str(uint8_t val)
{
    switch (val) {
    case WT_UPDATE_INVALID:
        return ("WT_UPDATE_INVALID");
    case WT_UPDATE_MODIFY:
        return ("WT_UPDATE_MODIFY");
    case WT_UPDATE_RESERVE:
        return ("WT_UPDATE_RESERVE");
    case WT_UPDATE_STANDARD:
        return ("WT_UPDATE_STANDARD");
    case WT_UPDATE_TOMBSTONE:
        return ("WT_UPDATE_TOMBSTONE");
    }

    return ("UPDATE_TYPE_INVALID");
}

/*
 * __wt_page_type_str --
 *     Convert a page type to its string representation.
 */
static inline const char *
__wt_page_type_str(uint8_t val)
{
    switch (val) {
    case WT_PAGE_INVALID:
        return ("WT_PAGE_INVALID");
    case WT_PAGE_BLOCK_MANAGER:
        return ("WT_PAGE_BLOCK_MANAGER");
    case WT_PAGE_COL_FIX:
        return ("WT_PAGE_COL_FIX");
    case WT_PAGE_COL_INT:
        return ("WT_PAGE_COL_INT");
    case WT_PAGE_COL_VAR:
        return ("WT_PAGE_COL_VAR");
    case WT_PAGE_OVFL:
        return ("WT_PAGE_OVFL");
    case WT_PAGE_ROW_INT:
        return ("WT_PAGE_ROW_INT");
    case WT_PAGE_ROW_LEAF:
        return ("WT_PAGE_ROW_LEAF");
    }

    return ("PAGE_TYPE_INVALID");
}
