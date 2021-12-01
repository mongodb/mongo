// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

extern crate num_cpus;

use std::env;
use std::process::{Command, Stdio};

fn main() {
    let out_dir = env::var("OUT_DIR").expect("Should have env var OUT_DIR");
    let target = env::var("TARGET").expect("Should have env var TARGET");

    let js_src = env::var("CARGO_MANIFEST_DIR").expect("Should have env var CARGO_MANIFEST_DIR");

    env::set_var("MAKEFLAGS", format!("-j{}", num_cpus::get()));
    env::set_current_dir(&js_src).unwrap();

    let variant = if cfg!(feature = "debugmozjs") {
        "plaindebug"
    } else {
        "plain"
    };

    let python = env::var("PYTHON").unwrap_or("python2.7".into());
    let mut cmd = Command::new(&python);
    cmd.args(&["./devtools/automation/autospider.py",
               // Only build SpiderMonkey, don't run all the tests.
               "--build-only",
               // Disable Mozilla's jemalloc; Rust has its own jemalloc that we
               // can swap in instead and everything using a single malloc is
               // good.
               "--no-jemalloc",
               "--objdir", &out_dir,
               variant])
        .env("SOURCE", &js_src)
        .env("PWD", &js_src)
        .env("AUTOMATION", "1")
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit());
    println!("Running command: {:?}", cmd);
    let result = cmd
        .status()
        .expect("Should spawn autospider OK");
    assert!(result.success(), "autospider should exit OK");

    println!("cargo:rustc-link-search=native={}/js/src/build", out_dir);
    println!("cargo:rustc-link-search=native={}/js/src", out_dir);
    println!("cargo:rustc-link-lib=static=js_static");

    println!("cargo:rustc-link-search=native={}/dist/bin", out_dir);
    println!("cargo:rustc-link-lib=nspr4");

    if target.contains("windows") {
        println!("cargo:rustc-link-lib=winmm");
        if target.contains("gnu") {
            println!("cargo:rustc-link-lib=stdc++");
        }
    } else {
        println!("cargo:rustc-link-lib=stdc++");
    }

    println!("cargo:outdir={}", out_dir);
}
