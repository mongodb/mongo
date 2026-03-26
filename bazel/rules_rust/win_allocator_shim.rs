// Allocator shim for linking Rust rlibs into a C++ binary on Windows.
//
// On Linux, rules_rust provides this via ffi/rs/allocator_library with
// #[linkage = "weak"], but MSVC doesn't support weak linkage. Since we're
// always linking into a C++ binary (never a Rust main), strong symbols are fine.
//
// See: https://github.com/rust-lang/rust/issues/73632
// See: rules_rust/ffi/rs/allocator_library/allocator_library.rs
#![no_std]
#![allow(warnings)]
#![allow(internal_features)]
#![feature(rustc_attrs)]

unsafe extern "C" {
    #[rustc_std_internal_symbol]
    fn __rdl_alloc(size: usize, align: usize) -> *mut u8;

    #[rustc_std_internal_symbol]
    fn __rdl_dealloc(ptr: *mut u8, size: usize, align: usize);

    #[rustc_std_internal_symbol]
    fn __rdl_realloc(ptr: *mut u8, old_size: usize, align: usize, new_size: usize) -> *mut u8;

    #[rustc_std_internal_symbol]
    fn __rdl_alloc_zeroed(size: usize, align: usize) -> *mut u8;
}

#[rustc_std_internal_symbol]
fn __rust_alloc(size: usize, align: usize) -> *mut u8 {
    unsafe { __rdl_alloc(size, align) }
}

#[rustc_std_internal_symbol]
fn __rust_dealloc(ptr: *mut u8, size: usize, align: usize) {
    unsafe { __rdl_dealloc(ptr, size, align) }
}

#[rustc_std_internal_symbol]
fn __rust_realloc(ptr: *mut u8, old_size: usize, align: usize, new_size: usize) -> *mut u8 {
    unsafe { __rdl_realloc(ptr, old_size, align, new_size) }
}

#[rustc_std_internal_symbol]
fn __rust_alloc_zeroed(size: usize, align: usize) -> *mut u8 {
    unsafe { __rdl_alloc_zeroed(size, align) }
}

#[rustc_std_internal_symbol]
static __rust_no_alloc_shim_is_unstable: u8 = 0;

#[rustc_std_internal_symbol]
fn __rust_no_alloc_shim_is_unstable_v2() {}
