/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2017 Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "rdkafka_int.h"
#include "rdkafka_plugin.h"
#include "rddl.h"


typedef struct rd_kafka_plugin_s {
        char *rkplug_path;     /* Library path */
        rd_kafka_t *rkplug_rk; /* Backpointer to the rk handle */
        void *rkplug_handle;   /* dlopen (or similar) handle */
        void *rkplug_opaque;   /* Plugin's opaque */

} rd_kafka_plugin_t;


/**
 * @brief Plugin path comparator
 */
static int rd_kafka_plugin_cmp(const void *_a, const void *_b) {
        const rd_kafka_plugin_t *a = _a, *b = _b;

        return strcmp(a->rkplug_path, b->rkplug_path);
}


/**
 * @brief Add plugin (by library path) and calls its conf_init() constructor
 *
 * @returns an error code on error.
 * @remark duplicate plugins are silently ignored.
 *
 * @remark Libraries are refcounted and thus not unloaded until all
 *         plugins referencing the library have been destroyed.
 *         (dlopen() and LoadLibrary() does this for us)
 */
static rd_kafka_resp_err_t rd_kafka_plugin_new(rd_kafka_conf_t *conf,
                                               const char *path,
                                               char *errstr,
                                               size_t errstr_size) {
        rd_kafka_plugin_t *rkplug;
        const rd_kafka_plugin_t skel = {.rkplug_path = (char *)path};
        rd_kafka_plugin_f_conf_init_t *conf_init;
        rd_kafka_resp_err_t err;
        void *handle;
        void *plug_opaque = NULL;

        /* Avoid duplicates */
        if (rd_list_find(&conf->plugins, &skel, rd_kafka_plugin_cmp)) {
                rd_snprintf(errstr, errstr_size, "Ignoring duplicate plugin %s",
                            path);
                return RD_KAFKA_RESP_ERR_NO_ERROR;
        }

        rd_kafka_dbg0(conf, PLUGIN, "PLUGLOAD", "Loading plugin \"%s\"", path);

        /* Attempt to load library */
        if (!(handle = rd_dl_open(path, errstr, errstr_size))) {
                rd_kafka_dbg0(conf, PLUGIN, "PLUGLOAD",
                              "Failed to load plugin \"%s\": %s", path, errstr);
                return RD_KAFKA_RESP_ERR__FS;
        }

        /* Find conf_init() function */
        if (!(conf_init =
                  rd_dl_sym(handle, "conf_init", errstr, errstr_size))) {
                rd_dl_close(handle);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        /* Call conf_init() */
        rd_kafka_dbg0(conf, PLUGIN, "PLUGINIT",
                      "Calling plugin \"%s\" conf_init()", path);

        if ((err = conf_init(conf, &plug_opaque, errstr, errstr_size))) {
                rd_dl_close(handle);
                return err;
        }

        rkplug                = rd_calloc(1, sizeof(*rkplug));
        rkplug->rkplug_path   = rd_strdup(path);
        rkplug->rkplug_handle = handle;
        rkplug->rkplug_opaque = plug_opaque;

        rd_list_add(&conf->plugins, rkplug);

        rd_kafka_dbg0(conf, PLUGIN, "PLUGLOAD", "Plugin \"%s\" loaded", path);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Free the plugin, any conf_destroy() interceptors will have been
 *        called prior to this call.
 * @remark plugin is not removed from any list (caller's responsibility)
 * @remark this relies on the actual library loader to refcount libraries,
 *         especially in the config copy case.
 *         This is true for POSIX dlopen() and Win32 LoadLibrary().
 * @locality application thread
 */
static void rd_kafka_plugin_destroy(rd_kafka_plugin_t *rkplug) {
        rd_dl_close(rkplug->rkplug_handle);
        rd_free(rkplug->rkplug_path);
        rd_free(rkplug);
}



/**
 * @brief Initialize all configured plugins.
 *
 * @remark Any previously loaded plugins will be unloaded.
 *
 * @returns the error code of the first failing plugin.
 * @locality application thread calling rd_kafka_new().
 */
static rd_kafka_conf_res_t rd_kafka_plugins_conf_set0(rd_kafka_conf_t *conf,
                                                      const char *paths,
                                                      char *errstr,
                                                      size_t errstr_size) {
        char *s;

        rd_list_destroy(&conf->plugins);
        rd_list_init(&conf->plugins, 0, (void *)&rd_kafka_plugin_destroy);

        if (!paths || !*paths)
                return RD_KAFKA_CONF_OK;

        /* Split paths by ; */
        rd_strdupa(&s, paths);

        rd_kafka_dbg0(conf, PLUGIN, "PLUGLOAD",
                      "Loading plugins from conf object %p: \"%s\"", conf,
                      paths);

        while (s && *s) {
                char *path = s;
                char *t;
                rd_kafka_resp_err_t err;

                if ((t = strchr(s, ';'))) {
                        *t = '\0';
                        s  = t + 1;
                } else {
                        s = NULL;
                }

                if ((err = rd_kafka_plugin_new(conf, path, errstr,
                                               errstr_size))) {
                        /* Failed to load plugin */
                        size_t elen = errstr_size > 0 ? strlen(errstr) : 0;

                        /* See if there is room for appending the
                         * plugin path to the error message. */
                        if (elen + strlen("(plugin )") + strlen(path) <
                            errstr_size)
                                rd_snprintf(errstr + elen, errstr_size - elen,
                                            " (plugin %s)", path);

                        rd_list_destroy(&conf->plugins);
                        return RD_KAFKA_CONF_INVALID;
                }
        }

        return RD_KAFKA_CONF_OK;
}


/**
 * @brief Conf setter for "plugin.library.paths"
 */
rd_kafka_conf_res_t rd_kafka_plugins_conf_set(int scope,
                                              void *pconf,
                                              const char *name,
                                              const char *value,
                                              void *dstptr,
                                              rd_kafka_conf_set_mode_t set_mode,
                                              char *errstr,
                                              size_t errstr_size) {

        assert(scope == _RK_GLOBAL);
        return rd_kafka_plugins_conf_set0(
            (rd_kafka_conf_t *)pconf,
            set_mode == _RK_CONF_PROP_SET_DEL ? NULL : value, errstr,
            errstr_size);
}
