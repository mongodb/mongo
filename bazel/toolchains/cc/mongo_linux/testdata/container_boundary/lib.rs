include!(concat!(env!("OUT_DIR"), "/generated.rs"));

pub const PROC_MACRO_VALUE: usize = boundary_macro::boundary_value!();

#[cfg(test)]
mod tests {
    use std::env;
    use std::path::Path;

    #[test]
    fn build_actions_were_containerized_but_test_execution_is_native() {
        assert_eq!(super::BUILD_SCRIPT_VALUE, 7);
        assert_eq!(super::PROC_MACRO_VALUE, 42);

        // The sentinel is supplied by run_boundary_test. Keep the ordinary Rust test
        // valid without the harness so `bazel test //...` remains safe.
        if let Ok(sentinel) = env::var("MONGO_CONTAINER_BOUNDARY_SENTINEL") {
            assert!(
                Path::new(&sentinel).exists(),
                "TestRunner did not execute natively on the host"
            );
        }
    }
}
