// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
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
