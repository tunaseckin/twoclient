fn main() {
    bundle_bedrock_mod();
    #[cfg(windows)]
    {
        let mut res = winresource::WindowsResource::new();
        res.set_icon("icons/icon.ico");

        let version = env!("CARGO_PKG_VERSION");
        res.set("CompanyName", "Polyfrost Inc.");
        res.set("ProductName", "TwoClient");
        res.set("FileDescription", "TwoClient");
        res.set("LegalCopyright", "© 2026 Polyfrost Inc.");
        res.set("OriginalFilename", "twoclient_app.exe");
        res.set("InternalName", "twoclient_app");
        res.set("FileVersion", version);
        res.set("ProductVersion", version);

        res.compile()
            .expect("failed to embed Windows icon resource");
    }
}

/// Carries the in-game Bedrock mod inside the app on the platforms that run it
/// through mcpelauncher. The mod is C++ built with the Android NDK beforehand
/// (see packages/bedrock_mod); without a build the app ships without it.
fn bundle_bedrock_mod() {
    let target = std::env::var("TARGET").unwrap_or_default();
    let abi = match target.as_str() {
        "aarch64-apple-darwin" | "aarch64-unknown-linux-gnu" => Some("arm64"),
        "x86_64-unknown-linux-gnu" => Some("x86_64"),
        _ => None,
    };
    let out = std::path::Path::new(&std::env::var("OUT_DIR").unwrap()).join("bedrock_mod.so");
    let source = std::env::var("BEDROCK_MOD_SO").ok().map(std::path::PathBuf::from).or_else(|| {
        abi.map(|abi| {
            std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../bedrock_mod/build")
                .join(abi)
                .join("libtwoclient_bedrock.so")
        })
    });
    println!("cargo:rerun-if-env-changed=BEDROCK_MOD_SO");
    match source.filter(|p| p.is_file()) {
        Some(path) => {
            println!("cargo:rerun-if-changed={}", path.display());
            std::fs::copy(&path, &out).expect("failed to copy the Bedrock mod");
        }
        None => std::fs::write(&out, []).expect("failed to write the empty Bedrock mod"),
    }
}
