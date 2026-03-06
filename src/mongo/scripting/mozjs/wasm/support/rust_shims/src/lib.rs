/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

// Rust shims for WASI linkage.
// Force-link the encoding shim crates so their exported C symbols are present.
extern crate encoding_c;
extern crate encoding_c_mem;

#[no_mangle]
pub extern "C" fn mongo_wasip2_rust_shims_keepalive() {
    // Touch at least one symbol from each crate so the linker keeps them.
    unsafe {
        let p = core::ptr::null::<u8>();
        let _ = encoding_c::encoding_utf8_valid_up_to(p, 0);
        let q = core::ptr::null::<u16>();
        let _ = encoding_c_mem::encoding_mem_is_utf8_latin1(p, 0);
        let _ = q;
    }
}
