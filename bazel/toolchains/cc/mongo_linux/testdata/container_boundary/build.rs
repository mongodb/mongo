use std::env;
use std::fs;
use std::path::Path;

fn main() {
    if let Ok(sentinel) = env::var("MONGO_CONTAINER_BOUNDARY_SENTINEL") {
        assert!(
            !Path::new(&sentinel).exists(),
            "Cargo build script escaped the action container and saw {sentinel}"
        );
    }

    let out_dir = env::var("OUT_DIR").expect("rules_rust supplies OUT_DIR");
    fs::write(
        Path::new(&out_dir).join("generated.rs"),
        "pub const BUILD_SCRIPT_VALUE: usize = 7;\n",
    )
    .expect("write generated Rust source");
}
