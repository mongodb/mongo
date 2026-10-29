extern crate proc_macro;

use proc_macro::TokenStream;

#[proc_macro]
pub fn boundary_value(_input: TokenStream) -> TokenStream {
    "42".parse().expect("valid Rust expression")
}
