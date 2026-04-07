# Add more methods to public API

Edit `mozjs.wit` to add new functions:

```
world api {
  export mozjstest: func() -> s32;
  export mynewfunc: func() -> s32;
}
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

The C bindings (`api.c`, `api.h`, `api_component_type.o`) are generated
automatically at build time by the `wit_bindgen_c` rule in
`src/mongo/scripting/mozjs/wasm/BUILD.bazel`.
