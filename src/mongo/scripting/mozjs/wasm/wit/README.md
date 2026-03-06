# Add more methods to public API:

```
world api {
  export mozjstest: func() -> s32;
  export mynewfunc: func() -> s32;
}
```

# Install wit-gen on your dev VM

```
cargo install --git https://github.com/bytecodealliance/wit-bindgen --locked wit-bindgen-cli
```

# Run the bindgen

```
cd src/mongo/scripting/mozjs/wasm
wit-bindgen c ./wit --out-dir wit_gen/generated
```

# Implement the symbol in engine/api.cpp

```
extern "C" int32_t exports_api_mozjstest(void) {
    return 1234;
}

extern "C" int32_t exports_api_mynewfunc(void) {
    return ...;
}
```

# Commit the generated files in wit_gen/generated
