
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

package com.mongodb.embedded.capi.internal;

import com.sun.jna.Callback;
import com.sun.jna.Memory;
import com.sun.jna.Native;
import com.sun.jna.NativeLong;
import com.sun.jna.Pointer;
import com.sun.jna.PointerType;
import com.sun.jna.Structure;
import com.sun.jna.ptr.NativeLongByReference;
import com.sun.jna.ptr.PointerByReference;

import java.util.Arrays;
import java.util.List;

//CHECKSTYLE:OFF
public class CAPI {

    public static class cstring extends PointerType {
        public cstring() {
            super();
        }

        public cstring(Pointer address) {
            super(address);
        }

        public cstring(String string) {
            Pointer m = new Memory(string.length() + 1);
            m.setString(0, string);
            setPointer(m);
        }

        public String toString() {
            return getPointer().getString(0);
        }
    }

    public static class mongo_embedded_v1_status extends PointerType {

        public mongo_embedded_v1_status() {
            super();
        }

        public mongo_embedded_v1_status(Pointer address) {
            super(address);
        }
    }

    public static class mongo_embedded_v1_lib extends PointerType {
        public mongo_embedded_v1_lib() {
            super();
        }

        public mongo_embedded_v1_lib(Pointer address) {
            super(address);
        }
    }

    public static class mongo_embedded_v1_instance extends PointerType {
        public mongo_embedded_v1_instance() {
            super();
        }

        public mongo_embedded_v1_instance(Pointer address) {
            super(address);
        }
    }

    public static class mongo_embedded_v1_client extends PointerType {
        public mongo_embedded_v1_client() {
            super();
        }

        public mongo_embedded_v1_client(Pointer address) {
            super(address);
        }
    }

    public static class mongo_embedded_v1_init_params extends Structure {
        public cstring yaml_config;
        public long log_flags;
        public mongo_embedded_v1_log_callback log_callback;
        public Pointer log_user_data;

        public mongo_embedded_v1_init_params() {
            super();
        }

        protected List<String> getFieldOrder() {
            return Arrays.asList("yaml_config", "log_flags", "log_callback",
                    "log_user_data");
        }

        public static class ByReference extends mongo_embedded_v1_init_params
                implements Structure.ByReference {
        }
    }

    public interface mongo_embedded_v1_log_callback extends Callback {
        void log(Pointer user_data, cstring message, cstring component, cstring context, int severity);
    }

    public static native mongo_embedded_v1_status mongo_embedded_v1_status_create();

    public static native void mongo_embedded_v1_status_destroy(mongo_embedded_v1_status status);

    public static native int mongo_embedded_v1_status_get_error(mongo_embedded_v1_status status);

    public static native cstring mongo_embedded_v1_status_get_explanation(mongo_embedded_v1_status status);

    public static native int mongo_embedded_v1_status_get_code(mongo_embedded_v1_status status);

    public static native mongo_embedded_v1_lib mongo_embedded_v1_lib_init(mongo_embedded_v1_init_params.ByReference init_params,
                                                                          mongo_embedded_v1_status status);

    public static native int mongo_embedded_v1_lib_fini(mongo_embedded_v1_lib lib, mongo_embedded_v1_status status);

    public static native mongo_embedded_v1_instance mongo_embedded_v1_instance_create(mongo_embedded_v1_lib lib, cstring yaml_config,
                                                                                      mongo_embedded_v1_status status);

    public static native int mongo_embedded_v1_instance_destroy(mongo_embedded_v1_instance instance, mongo_embedded_v1_status status);

    public static native mongo_embedded_v1_client mongo_embedded_v1_client_create(mongo_embedded_v1_instance instance,
                                                                                  mongo_embedded_v1_status status);

    public static native int mongo_embedded_v1_client_destroy(mongo_embedded_v1_client client, mongo_embedded_v1_status status);

    public static native int mongo_embedded_v1_client_invoke(mongo_embedded_v1_client client, Pointer input, NativeLong size,
                                                             PointerByReference output, NativeLongByReference output_size,
                                                             mongo_embedded_v1_status status);

    static {
        Native.register(CAPI.class, "mongo_embedded");
    }

}
//CHECKSTYLE:ON
