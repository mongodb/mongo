/* Copyright 2021 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

extern crate arbitrary;
extern crate wasm_smith;

use arbitrary::{Arbitrary, Unstructured};
use wasm_smith::{Config, Module};

use std::ptr;

// A custom configuration to enable all experimental features that we have
// some support for.
#[derive(Arbitrary, Debug)]
struct SpiderMonkeyConfig;

impl Config for SpiderMonkeyConfig {
    fn bulk_memory_enabled(&self) -> bool {
        true
    }
    fn reference_types_enabled(&self) -> bool {
        true
    }
    fn exceptions_enabled(&self) -> bool {
        true
    }
    fn memory64_enabled(&self) -> bool {
        true
    }
    fn simd_enabled(&self) -> bool {
        true
    }
}

#[no_mangle]
pub unsafe extern "C" fn gluesmith(
    data: *mut u8,
    len: usize,
    out: *mut u8,
    maxlen: usize,
) -> usize {
    let buf: &[u8] = std::slice::from_raw_parts(data, len);

    let mut u = Unstructured::new(buf);

    let module = match Module::new(SpiderMonkeyConfig {}, &mut u) {
        Ok(m) => m,
        Err(_e) => return 0,
    };

    let wasm_bytes = module.to_bytes();

    let src_len = wasm_bytes.len();

    if src_len > maxlen {
        return 0;
    }

    let src_ptr = wasm_bytes.as_ptr();
    ptr::copy_nonoverlapping(src_ptr, out, src_len);

    return src_len;
}
