fn main() {
    cxx_build::bridge("src/bridge.rs")
        .std("c++17")
        .compile("logger_cxx");
    println!("cargo:rerun-if-changed=src/bridge.rs");
}
