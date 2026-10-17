fn main() {
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
