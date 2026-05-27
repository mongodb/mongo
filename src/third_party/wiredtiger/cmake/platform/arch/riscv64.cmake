# See https://www.sifive.com/blog/all-aboard-part-1-compiler-args
# for background on the `rv64imafdc` and `lp64d` arguments here.
add_compile_options(-march=rv64imafdc -mabi=lp64d)
