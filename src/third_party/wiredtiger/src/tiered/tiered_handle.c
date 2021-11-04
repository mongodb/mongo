/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __tiered_dhandle_setup --
 *     Given a tiered index and name, set up the dhandle information.
 */
static int
__tiered_dhandle_setup(WT_SESSION_IMPL *session, WT_TIERED *tiered, uint32_t i, const char *name)
{
    WT_DECL_RET;
    WT_TIERED_TIERS *tier;
    uint32_t id, type;

    WT_RET(__wt_session_get_dhandle(session, name, NULL, NULL, 0));
    if (i == WT_TIERED_INDEX_INVALID) {
        type = session->dhandle->type;
        if (type == WT_DHANDLE_TYPE_BTREE)
            id = WT_TIERED_INDEX_LOCAL;
        else if (type == WT_DHANDLE_TYPE_TIERED)
            id = WT_TIERED_INDEX_LOCAL;
        else if (type == WT_DHANDLE_TYPE_TIERED_TREE)
            /*
             * FIXME-WT-7538: this type can be removed. For now, there is nothing to do for this
             * type.
             */
            goto err;
        else
            WT_ERR_MSG(
              session, EINVAL, "Unknown or unsupported tiered dhandle type %" PRIu32, type);
    } else {
        WT_ASSERT(session, i < WT_TIERED_MAX_TIERS);
        id = i;
    }
    /* Reference the dhandle and set it in the tier array. */
    tier = &tiered->tiers[id];
    (void)__wt_atomic_addi32(&session->dhandle->session_inuse, 1);
    tier->tier = session->dhandle;

    /* The Btree needs to use the bucket storage to do file system operations. */
    if (session->dhandle->type == WT_DHANDLE_TYPE_BTREE)
        ((WT_BTREE *)session->dhandle->handle)->bstorage = tiered->bstorage;
err:
    WT_RET(__wt_session_release_dhandle(session));
    return (ret);
}

/*
 * __tiered_init_tiers --
 *     Given a tiered table 'tiers' configuration set up the dhandle array.
 */
static int
__tiered_init_tiers(WT_SESSION_IMPL *session, WT_TIERED *tiered, WT_CONFIG_ITEM *tierconf)
{
    WT_CONFIG cparser;
    WT_CONFIG_ITEM ckey, cval;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_TIERED_TIERS *local_tier;

    WT_RET(__wt_scr_alloc(session, 0, &tmp));
    __wt_config_subinit(session, &cparser, tierconf);
    while ((ret = __wt_config_next(&cparser, &ckey, &cval)) == 0) {
        /* Set up the tiers array based on the metadata. */
        WT_ERR(__wt_buf_fmt(session, tmp, "%.*s", (int)ckey.len, ckey.str));
        __wt_verbose(
          session, WT_VERB_TIERED, "INIT_TIERS: tiered URI dhandle %s", (char *)tmp->data);
        WT_SAVE_DHANDLE(session,
          ret = __tiered_dhandle_setup(
            session, tiered, WT_TIERED_INDEX_INVALID, (const char *)tmp->data));
        WT_ERR(ret);
    }
    local_tier = &tiered->tiers[WT_TIERED_INDEX_LOCAL];
    if (local_tier->name == NULL) {
        WT_ERR(__wt_tiered_name(
          session, &tiered->iface, tiered->current_id, WT_TIERED_NAME_LOCAL, &local_tier->name));
        F_SET(local_tier, WT_TIERS_OP_READ | WT_TIERS_OP_WRITE);
    }
    WT_ERR_NOTFOUND_OK(ret, false);
err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __tiered_create_local --
 *     Create a new local name for a tiered table. Must be called single threaded.
 */
static int
__tiered_create_local(WT_SESSION_IMPL *session, WT_TIERED *tiered)
{
    WT_CONFIG cparser;
    WT_CONFIG_ITEM ck, cv;
    WT_DECL_ITEM(build);
    WT_DECL_RET;
    WT_TIERED_TIERS *this_tier;
    const char *cfg[4] = {NULL, NULL, NULL, NULL};
    const char *config, *name;

    config = name = NULL;

    /* If this ever can be multi-threaded, this would need to be atomic. */
    tiered->current_id = tiered->next_id++;
    /* XXX Remove when we have real flags. */
    F_SET(tiered, WT_TIERED_FLAG_UNUSED);
    WT_ERR(
      __wt_tiered_name(session, &tiered->iface, tiered->current_id, WT_TIERED_NAME_LOCAL, &name));
    __wt_verbose(session, WT_VERB_TIERED, "TIER_CREATE_LOCAL: LOCAL: %s", name);
    cfg[0] = WT_CONFIG_BASE(session, object_meta);
    cfg[1] = tiered->obj_config;
    cfg[2] = "tiered_object=true,readonly=true";
    __wt_verbose(session, WT_VERB_TIERED, "TIER_CREATE_LOCAL: obj_config: %s : %s", name, cfg[1]);
    WT_ASSERT(session, tiered->obj_config != NULL);
    WT_ERR(__wt_config_merge(session, cfg, NULL, (const char **)&config));

    /*
     * Remove any checkpoint entry from the configuration. The local file we are now creating is
     * empty and does not have any checkpoints.
     */
    WT_ERR(__wt_scr_alloc(session, 1024, &build));
    __wt_config_init(session, &cparser, config);
    while ((ret = __wt_config_next(&cparser, &ck, &cv)) == 0) {
        if (!WT_STRING_MATCH("checkpoint", ck.str, ck.len))
            /* Append the entry to the new buffer. */
            WT_ERR(__wt_buf_catfmt(
              session, build, "%.*s=%.*s,", (int)ck.len, ck.str, (int)cv.len, cv.str));
    }
    __wt_free(session, config);
    WT_ERR(__wt_strndup(session, build->data, build->size, &config));

    /*
     * XXX Need to verify user doesn't create a table of the same name. What does LSM do? It
     * definitely has the same problem with chunks.
     */
    __wt_verbose(
      session, WT_VERB_TIERED, "TIER_CREATE_LOCAL: schema create LOCAL: %s : %s", name, config);
    WT_ERR(__wt_schema_create(session, name, config));
    this_tier = &tiered->tiers[WT_TIERED_INDEX_LOCAL];
    if (this_tier->name != NULL)
        __wt_free(session, this_tier->name);
    this_tier->name = name;
    F_SET(this_tier, WT_TIERS_OP_READ | WT_TIERS_OP_WRITE);

    WT_WITH_DHANDLE(
      session, &tiered->iface, ret = __wt_btree_switch_object(session, tiered->current_id, 0));
    WT_ERR(ret);

err:
    __wt_scr_free(session, &build);
    if (ret != 0)
        /* Only free name on error. */
        __wt_free(session, name);

    __wt_free(session, config);
    return (ret);
}

/*
 * __tiered_create_object --
 *     Create an object name of the given number.
 */
static int
__tiered_create_object(WT_SESSION_IMPL *session, WT_TIERED *tiered)
{
    WT_DECL_RET;
    const char *cfg[4] = {NULL, NULL, NULL, NULL};
    const char *config, *name, *orig_name;

    config = name = NULL;
    config = name = orig_name = NULL;
    orig_name = tiered->tiers[WT_TIERED_INDEX_LOCAL].name;
    /*
     * If we have an existing local file in the tier, alter the table to indicate this one is now
     * readonly. We are already holding the schema lock so we can call alter.
     */
    if (orig_name != NULL) {
        cfg[0] = "readonly=true";
        WT_WITHOUT_DHANDLE(session, ret = __wt_schema_alter(session, orig_name, cfg));
        WT_ERR(ret);
    }
    /*
     * Create the name and metadata of the new shared object of the current local object. The data
     * structure keeps this id so that we don't have to parse and manipulate strings.
     */
    WT_ERR(
      __wt_tiered_name(session, &tiered->iface, tiered->current_id, WT_TIERED_NAME_OBJECT, &name));
    cfg[0] = WT_CONFIG_BASE(session, object_meta);
    cfg[1] = tiered->obj_config;
    cfg[2] = "flush=0,readonly=true";
    WT_ASSERT(session, tiered->obj_config != NULL);
    WT_ERR(__wt_config_merge(session, cfg, NULL, (const char **)&config));
    __wt_verbose(
      session, WT_VERB_TIERED, "TIER_CREATE_OBJECT: schema create %s : %s", name, config);
    /* Create the new shared object. */
    WT_ERR(__wt_schema_create(session, name, config));

err:
    __wt_free(session, config);
    __wt_free(session, name);
    return (ret);
}

/*
 * __tiered_create_tier_tree --
 *     Create a tier name for a tiered table.
 */
static int
__tiered_create_tier_tree(WT_SESSION_IMPL *session, WT_TIERED *tiered)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_TIERED_TIERS *this_tier;
    const char *cfg[4] = {NULL, NULL, NULL, NULL};
    const char *config, *name;

    config = name = NULL;
    WT_RET(__wt_scr_alloc(session, 0, &tmp));

    /* Create tier:example for the new tiered tree. */
    WT_ERR(__wt_tiered_name(session, &tiered->iface, 0, WT_TIERED_NAME_SHARED, &name));
    cfg[0] = WT_CONFIG_BASE(session, tier_meta);
    WT_ASSERT(session, tiered->bstorage != NULL);
    WT_ERR(__wt_buf_fmt(session, tmp, ",readonly=true,tiered_storage=(bucket=%s,bucket_prefix=%s)",
      tiered->bstorage->bucket, tiered->bstorage->bucket_prefix));
    cfg[2] = tmp->data;
    WT_ERR(__wt_config_merge(session, cfg, NULL, &config));
    /* Set up a tier:example metadata for the first time. */
    __wt_verbose(session, WT_VERB_TIERED, "CREATE_TIER_TREE: schema create: %s : %s", name, config);
    WT_ERR(__wt_schema_create(session, name, config));
    this_tier = &tiered->tiers[WT_TIERED_INDEX_SHARED];
    WT_ASSERT(session, this_tier->name == NULL);
    this_tier->name = name;
    F_SET(this_tier, WT_TIERS_OP_FLUSH | WT_TIERS_OP_READ);

    if (0)
err:
        /* Only free on error. */
        __wt_free(session, name);
    __wt_free(session, config);
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __tiered_update_dhandles --
 *     Update the dhandle list for a tiered structure after object switching.
 */
static int
__tiered_update_dhandles(WT_SESSION_IMPL *session, WT_TIERED *tiered)
{
    WT_DECL_RET;
    uint32_t i;

    /* Now get the dhandle and add it to the array. */
    for (i = 0; i < WT_TIERED_MAX_TIERS; ++i) {
        /*
         * If we have a tiered dhandle we can either skip if it is the same name or we decrement the
         * old one and get a new one for the new name.
         */
        if (tiered->tiers[i].tier != NULL) {
            WT_ASSERT(session, tiered->tiers[i].name != NULL);
            if (strcmp(tiered->tiers[i].tier->name, tiered->tiers[i].name) == 0)
                continue;
            else
                (void)__wt_atomic_subi32(&tiered->tiers[i].tier->session_inuse, 1);
        }
        if (tiered->tiers[i].name == NULL)
            continue;
        __wt_verbose(
          session, WT_VERB_TIERED, "UPDATE_DH: Get dhandle for %s", tiered->tiers[i].name);
        WT_ERR(__tiered_dhandle_setup(session, tiered, i, tiered->tiers[i].name));
    }
err:
    __wt_verbose(session, WT_VERB_TIERED, "UPDATE_DH: DONE ret %d", ret);
    if (ret != 0) {
        /* Need to undo our dhandles. Close and dereference all. */
        for (i = 0; i < WT_TIERED_MAX_TIERS; ++i) {
            if (tiered->tiers[i].tier != NULL)
                (void)__wt_atomic_subi32(&tiered->tiers[i].tier->session_inuse, 1);
            __wt_free(session, tiered->tiers[i].name);
            tiered->tiers[i].tier = NULL;
            tiered->tiers[i].name = NULL;
        }
    }
    return (ret);
}

/*
 * __wt_tiered_set_metadata --
 *     Generate the tiered metadata information string into the given buffer.
 */
int
__wt_tiered_set_metadata(WT_SESSION_IMPL *session, WT_TIERED *tiered, WT_ITEM *buf)
{
    uint32_t i;

    WT_RET(__wt_buf_catfmt(session, buf, ",last=%" PRIu32 ",oldest=%" PRIu32 ",tiers=(",
      tiered->current_id, tiered->oldest_id));
    for (i = 0; i < WT_TIERED_MAX_TIERS; ++i) {
        if (tiered->tiers[i].name == NULL) {
            __wt_verbose(session, WT_VERB_TIERED, "TIER_SET_META: names[%" PRIu32 "] NULL", i);
            continue;
        }
        __wt_verbose(session, WT_VERB_TIERED, "TIER_SET_META: names[%" PRIu32 "]: %s", i,
          tiered->tiers[i].name);
        WT_RET(__wt_buf_catfmt(session, buf, "%s\"%s\"", i == 0 ? "" : ",", tiered->tiers[i].name));
    }
    WT_RET(__wt_buf_catfmt(session, buf, ")"));
    return (0);
}

/*
 * __tiered_update_metadata --
 *     Update the metadata for a tiered structure after object switching.
 */
static int
__tiered_update_metadata(WT_SESSION_IMPL *session, WT_TIERED *tiered, const char *orig_config)
{
    WT_DATA_HANDLE *dhandle;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    const char *cfg[4] = {NULL, NULL, NULL, NULL};
    const char *newconfig;

    dhandle = &tiered->iface;
    newconfig = NULL;
    WT_RET(__wt_scr_alloc(session, 0, &tmp));

    WT_ERR(__wt_tiered_set_metadata(session, tiered, tmp));

    cfg[0] = WT_CONFIG_BASE(session, tiered_meta);
    cfg[1] = orig_config;
    cfg[2] = tmp->data;
    WT_ERR(__wt_config_merge(session, cfg, NULL, &newconfig));
    __wt_verbose(
      session, WT_VERB_TIERED, "TIER_UPDATE_META: Update TIERED: %s %s", dhandle->name, newconfig);
    WT_ERR(__wt_metadata_update(session, dhandle->name, newconfig));

err:
    __wt_free(session, newconfig);
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __tiered_switch --
 *     Given a tiered table, make all the metadata updates underneath to switch to the next object.
 *     The switch handles going from nothing to local-only, local-only to both local and shared, and
 *     having shared-only and creating a local object. Must be single threaded.
 */
static int
__tiered_switch(WT_SESSION_IMPL *session, const char *config)
{
    WT_DECL_RET;
    WT_TIERED *tiered;
    bool need_object, need_tree, tracking;

    tiered = (WT_TIERED *)session->dhandle;
    __wt_verbose(
      session, WT_VERB_TIERED, "TIER_SWITCH: called %s %s", session->dhandle->name, config);

    need_object = tiered->tiers[WT_TIERED_INDEX_LOCAL].tier != NULL;
    need_tree = need_object && tiered->tiers[WT_TIERED_INDEX_SHARED].tier == NULL;
    /*
     * There are four possibilities to our tiers configuration. In all of them we need to create
     * a new local tier file object dhandle and add it as element index zero of the tiers array.
     * Then we may or may not do other operations depending on the state otherwise. These are
     * presented in order of increasing amount of work that needs to be done.
     *   1. tiers=() - New and empty. We only need to add in the local file object.
     *   2. tiers=("tier:...") - Existing shared tier only. Here too we only need to add
     *      in the local file object.
     *   3. tiers=("file:...", "tier:...") - Both local and shared tiers exist in the metadata.
     *       We need to create and add the next local file object (N + 1) and create a shared
     * object in the metadata for the current local file object (N).
     *   4. tiers=("file:...") - Existing local tier only. We need to do all of the parts listed
     * in the #3 above, and also create the shared tier metadata entry.
     *
     * Step 4 must be done after some order of 1-3.
     *   1. Create the "object:" entry in metadata if needed.
     *   2. Create the "tier:" entry in metadata if needed.
     *   3. Create the new "file:" local entry in metadata.
     *   4. Update the "tiered:" with new tiers and object number.
     *   5. Meta tracking off to "commit" all the metadata operations.
     *   6. Revise the dhandles in the tiered structure to reflect new state of the world.
     */

    /*
     * To be implemented with flush_tier:
     *    - Close the current object.
     *    - Copy the current one to the cloud. It also remains in the local store.
     */

    WT_RET(__wt_meta_track_on(session));
    tracking = true;
    if (need_tree)
        WT_ERR(__tiered_create_tier_tree(session, tiered));

    /* Create the object: entry in the metadata. */
    if (need_object) {
        WT_ERR(__tiered_create_object(session, tiered));
        WT_ERR(__wt_tiered_put_flush(session, tiered));
    }

    /* We always need to create a local object. */
    WT_ERR(__tiered_create_local(session, tiered));

    /* Update the tiered: metadata to new object number and tiered array. */
    WT_ERR(__tiered_update_metadata(session, tiered, config));
    tracking = false;
    WT_ERR(__wt_meta_track_off(session, true, ret != 0));
    WT_ERR(__tiered_update_dhandles(session, tiered));
err:
    __wt_verbose(session, WT_VERB_TIERED, "TIER_SWITCH: DONE ret %d", ret);
    if (tracking)
        WT_RET(__wt_meta_track_off(session, true, ret != 0));
    return (ret);
}

/*
 * __wt_tiered_switch --
 *     Switch metadata, external version.
 */
int
__wt_tiered_switch(WT_SESSION_IMPL *session, const char *config)
{
    WT_DECL_RET;

    /*
     * For now just a wrapper to internal function. Maybe there's more to do externally, like wrap
     * it in a lock or with a dhandle or walk the dhandle list here rather than higher up.
     */
    WT_SAVE_DHANDLE(session, ret = __tiered_switch(session, config));
    return (ret);
}

/*
 * __wt_tiered_name --
 *     Given a dhandle structure and object number generate the URI name of the given type.
 */
int
__wt_tiered_name(
  WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle, uint32_t id, uint32_t flags, const char **retp)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    const char *name;

    WT_RET(__wt_scr_alloc(session, 0, &tmp));
    name = dhandle->name;
    /* Skip the prefix depending on what we're given. */
    if (dhandle->type == WT_DHANDLE_TYPE_TIERED)
        WT_PREFIX_SKIP_REQUIRED(session, name, "tiered:");
    else {
        WT_ASSERT(session, dhandle->type == WT_DHANDLE_TYPE_TIERED_TREE);
        WT_ASSERT(session, !LF_ISSET(WT_TIERED_NAME_SHARED));
        WT_PREFIX_SKIP_REQUIRED(session, name, "tier:");
    }

    /*
     * Separate object numbers from the base table name with a dash. Separate from the suffix with a
     * dot. We generate a different name style based on the type.
     */
    if (LF_ISSET(WT_TIERED_NAME_LOCAL)) {
        if (LF_ISSET(WT_TIERED_NAME_PREFIX))
            WT_ERR(__wt_buf_fmt(session, tmp, "file:%s-", name));
        else
            WT_ERR(__wt_buf_fmt(session, tmp, "file:%s-%010" PRIu32 ".wtobj", name, id));
    } else if (LF_ISSET(WT_TIERED_NAME_OBJECT)) {
        if (LF_ISSET(WT_TIERED_NAME_PREFIX))
            WT_ERR(__wt_buf_fmt(session, tmp, "object:%s-", name));
        else
            WT_ERR(__wt_buf_fmt(session, tmp, "object:%s-%010" PRIu32 ".wtobj", name, id));
    } else {
        WT_ASSERT(session, !LF_ISSET(WT_TIERED_NAME_PREFIX));
        WT_ASSERT(session, LF_ISSET(WT_TIERED_NAME_SHARED));
        WT_ERR(__wt_buf_fmt(session, tmp, "tier:%s", name));
    }
    WT_ERR(__wt_strndup(session, tmp->data, tmp->size, retp));
    __wt_verbose(session, WT_VERB_TIERED, "Generated tiered name: %s", *retp);
err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __tiered_open --
 *     Open a tiered data handle (internal version).
 */
static int
__tiered_open(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONFIG_ITEM cval, tierconf;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_TIERED *tiered;
    WT_TIERED_WORK_UNIT *entry;
    uint32_t unused;
    char *metaconf;
    const char *obj_cfg[] = {WT_CONFIG_BASE(session, object_meta), NULL, NULL};
    const char **tiered_cfg, *config;

    dhandle = session->dhandle;
    tiered = (WT_TIERED *)dhandle;
    tiered_cfg = dhandle->cfg;
    config = NULL;
    metaconf = NULL;
    WT_RET(__wt_scr_alloc(session, 0, &tmp));

    WT_UNUSED(cfg);

    /* Set up the bstorage from the configuration first. */
    WT_RET(__wt_config_gets(session, tiered_cfg, "tiered_storage.name", &cval));
    if (cval.len == 0)
        tiered->bstorage = S2C(session)->bstorage;
    else
        WT_ERR(__wt_tiered_bucket_config(session, tiered_cfg, &tiered->bstorage));
    WT_ASSERT(session, tiered->bstorage != NULL);
    /* Collapse into one string for later use in switch. */
    WT_ERR(__wt_config_merge(session, tiered_cfg, NULL, &config));

    /*
     * Pull in any configuration of the original table for the object and file components that may
     * have been sent in on the create.
     */
    obj_cfg[1] = config;
    WT_ERR(__wt_config_collapse(session, obj_cfg, &metaconf));
    tiered->obj_config = metaconf;
    metaconf = NULL;
    __wt_verbose(session, WT_VERB_TIERED, "TIERED_OPEN: obj_config %s", tiered->obj_config);

    WT_ERR(__wt_config_getones(session, config, "key_format", &cval));
    WT_ERR(__wt_strndup(session, cval.str, cval.len, &tiered->key_format));
    WT_ERR(__wt_config_getones(session, config, "value_format", &cval));
    WT_ERR(__wt_strndup(session, cval.str, cval.len, &tiered->value_format));

    WT_ERR(__wt_config_getones(session, config, "last", &cval));
    tiered->current_id = (uint32_t)cval.val;
    tiered->next_id = tiered->current_id + 1;
    /*
     * For now this is always one. When garbage collection gets implemented then it will be updated
     * to reflect the first object number that exists. Knowing this information will be helpful for
     * other tasks such as tiered backup.
     */
    WT_ERR(__wt_config_getones(session, config, "oldest", &cval));
    tiered->oldest_id = (uint32_t)cval.val;
    WT_ASSERT(session, tiered->oldest_id == 1);

    __wt_verbose(session, WT_VERB_TIERED,
      "TIERED_OPEN: current %" PRIu32 ", next %" PRIu32 ", oldest %" PRIu32, tiered->current_id,
      tiered->next_id, tiered->oldest_id);

    ret = __wt_config_getones(session, config, "tiers", &tierconf);
    WT_ERR_NOTFOUND_OK(ret, true);

    /* Open tiers if we have them, otherwise initialize. */
    if (tiered->current_id != 0)
        WT_ERR(__tiered_init_tiers(session, tiered, &tierconf));
    else {
        __wt_verbose(
          session, WT_VERB_TIERED, "TIERED_OPEN: create %s config %s", dhandle->name, config);
        WT_ERR(__wt_tiered_switch(session, config));
    }
    WT_ERR(__wt_btree_open(session, tiered_cfg));
    WT_ERR(__wt_btree_switch_object(session, tiered->current_id, 0));

#if 1
    if (0) {
        /* Temp code to keep s_all happy. */
        FLD_SET(unused, WT_TIERED_OBJ_LOCAL | WT_TIERED_TREE_UNUSED);
        FLD_SET(unused, WT_TIERED_WORK_FORCE | WT_TIERED_WORK_FREE);
        WT_ERR(__wt_tiered_put_drop_shared(session, tiered, tiered->current_id));
        __wt_tiered_get_drop_shared(session, &entry);
    }
#endif

    if (0) {
err:
        __wt_free(session, tiered->obj_config);
        __wt_free(session, metaconf);
    }
    __wt_verbose(session, WT_VERB_TIERED, "TIERED_OPEN: Done ret %d", ret);
    __wt_scr_free(session, &tmp);
    __wt_free(session, config);
    return (ret);
}

/*
 * __wt_tiered_open --
 *     Open a tiered data handle.
 */
int
__wt_tiered_open(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_DECL_RET;

    WT_WITH_TXN_ISOLATION(session, WT_ISO_READ_UNCOMMITTED, ret = __tiered_open(session, cfg));

    return (ret);
}

/*
 * __wt_tiered_close --
 *     Close a tiered data handle.
 */
int
__wt_tiered_close(WT_SESSION_IMPL *session)
{
    return (__wt_btree_close(session));
}

/*
 * __wt_tiered_discard --
 *     Discard a tiered data handle.
 */
int
__wt_tiered_discard(WT_SESSION_IMPL *session, WT_TIERED *tiered)
{
    uint32_t i;

    __wt_free(session, tiered->key_format);
    __wt_free(session, tiered->value_format);
    __wt_free(session, tiered->obj_config);
    __wt_verbose(session, WT_VERB_TIERED, "%s", "TIERED_CLOSE: called");
    /*
     * For the moment we don't have anything to return. But all the callers currently expect a real
     * return value from a close function. And this may become more complex later. During connection
     * close the other dhandles may be closed and freed before this dhandle. So just free the names.
     */
    for (i = 0; i < WT_TIERED_MAX_TIERS; i++) {
        if (tiered->tiers[i].name != NULL)
            __wt_free(session, tiered->tiers[i].name);
    }

    return (__wt_btree_discard(session));
}

/*
 * __wt_tiered_tree_open --
 *     Open a tiered tree data handle.
 */
int
__wt_tiered_tree_open(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *key, *object, *value;

    WT_UNUSED(cfg);
    object = NULL;
    /*
     * Set dhandle->handle with tiered tree structure, initialized.
     */
    __wt_verbose(session, WT_VERB_TIERED, "TIERED_TREE_OPEN: Called %s", session->dhandle->name);
    WT_ASSERT(session, session->dhandle != NULL);
    WT_RET(__wt_metadata_cursor(session, &cursor));
    WT_ERR(__wt_tiered_name(
      session, session->dhandle, 0, WT_TIERED_NAME_OBJECT | WT_TIERED_NAME_PREFIX, &object));
    /*
     * Walk looking for our objects.
     */
    while (cursor->next(cursor) == 0) {
        cursor->get_key(cursor, &key);
        cursor->get_value(cursor, &value);
        /*
         * NOTE: Here we do anything we need to do to open or access each shared object.
         */
        if (!WT_STRING_MATCH(key, object, strlen(object)))
            continue;
        __wt_verbose(
          session, WT_VERB_TIERED, "TIERED_TREE_OPEN: metadata for %s: %s", object, value);
    }
err:
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    __wt_free(session, object);
    return (ret);
}

/*
 * __wt_tiered_tree_close --
 *     Close a tiered tree data handle.
 */
int
__wt_tiered_tree_close(WT_SESSION_IMPL *session, WT_TIERED_TREE *tiered_tree)
{
    WT_DECL_RET;

    __wt_verbose(session, WT_VERB_TIERED, "TIERED_TREE_CLOSE: called %s", tiered_tree->iface.name);
    ret = 0;
    __wt_free(session, tiered_tree->key_format);
    __wt_free(session, tiered_tree->value_format);

    return (ret);
}
