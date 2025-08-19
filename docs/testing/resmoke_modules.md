# Resmoke Module Configuration

This configuration allows additional modules to be added to Resmoke, providing more context about their associated directories. Modules can specify directories for fixtures, hooks, suites, and JavaScript tests, which Resmoke incorporates during its testing process.

## Adding a New Module

To add a new module to Resmoke, define the module name and specify its `fixture_dirs`, `hook_dirs`, `suite_dirs`, and `jstest_dirs` in the YAML configuration. Each field should be a list of directory paths.

### Example YAML Configuration

```yaml
my_new_module:
  fixture_dirs:
    - path/to/my_new_module/fixtures
  hook_dirs:
    - path/to/my_new_module/hooks
  suite_dirs:
    - path/to/my_new_module/suites
  jstest_dirs:
    - path/to/my_new_module/jstests
```

### Field Descriptions

- **`fixture_dirs`**: Directories containing fixtures associated with the module.
- **`hook_dirs`**: Directories containing hooks associated with the module.
- **`suite_dirs`**: Directories containing suites with test configurations.
- **`jstest_dirs`**: Directories containing JavaScript tests specific to the module. This ensures module-specific tests are excluded from other suite configurations when the module is disabled.

## Notes

- Any suite can use jstests from any directory, when the module is enabled the configured jstest dirs does nothing. Only when the module is disabled does it filter out the tests that might be configured in a suite from a different module.
- Fields can be omitted or empty lists
