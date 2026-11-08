//! Finds and starts Bedrock Edition. Windows has the real game, macOS and Linux
//! run the Android build through mcpelauncher.

use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};

pub const MCPELAUNCHER_URL: &str = "https://mcpelauncher.readthedocs.io/en/latest/getting_started/index.html";

#[cfg(target_os = "macos")]
const MAC_APP: &str = "Minecraft Bedrock Launcher.app";
#[cfg(target_os = "linux")]
const FLATPAK_ID: &str = "io.mrarm.mcpelauncher";

#[derive(Clone, Debug, PartialEq)]
pub enum Install {
    #[cfg(target_os = "macos")]
    MacApp(PathBuf),
    /// Directory holding `mcpelauncher-ui-qt` and `mcpelauncher-client`
    #[cfg(target_os = "linux")]
    Binary(PathBuf),
    #[cfg(target_os = "linux")]
    Flatpak,
    #[cfg(target_os = "windows")]
    Windows,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Bedrock {
    pub install: Option<Install>,
    /// mcpelauncher's own data folder, holding `versions` and the default `games`
    pub root: Option<PathBuf>,
    /// Newest game version mcpelauncher has downloaded and extracted
    pub version: Option<PathBuf>,
    /// The game's `com.mojang` folder, once the game has run at least once
    pub data_dir: Option<PathBuf>,
}

pub fn detect() -> Bedrock {
    let home = directories::BaseDirs::new();
    let home = home.as_ref();
    let install = find_install(home);
    let roots = data_roots(home, install.as_ref());
    let version = roots.iter().find_map(|root| newest_version(&root.join("versions")));
    let root = version
        .as_ref()
        .and_then(|v| v.parent()?.parent().map(Path::to_path_buf))
        .or_else(|| roots.first().cloned());
    let data_dir = roots
        .iter()
        .map(|root| root.join("games/com.mojang"))
        .find(|dir| dir.is_dir());
    Bedrock { install, root, version, data_dir }
}

#[cfg(target_os = "macos")]
fn find_install(home: Option<&directories::BaseDirs>) -> Option<Install> {
    let managed = managed_dir().map(|dir| dir.join(MAC_APP)).filter(|app| app.is_dir());
    if let Some(app) = managed {
        return Some(Install::MacApp(app));
    }
    let user_apps = home.map(|h| h.home_dir().join("Applications"));
    [Some(PathBuf::from("/Applications")), user_apps]
        .into_iter()
        .flatten()
        .map(|dir| dir.join(MAC_APP))
        .find(|app| app.is_dir())
        .map(Install::MacApp)
}

#[cfg(target_os = "linux")]
fn find_install(home: Option<&directories::BaseDirs>) -> Option<Install> {
    if let Some(dir) = find_on_path("mcpelauncher-ui-qt") {
        return Some(Install::Binary(dir));
    }
    let user_flatpak = home.map(|h| h.data_dir().join("flatpak/app").join(FLATPAK_ID));
    [Some(PathBuf::from("/var/lib/flatpak/app").join(FLATPAK_ID)), user_flatpak]
        .into_iter()
        .flatten()
        .any(|dir| dir.is_dir())
        .then_some(Install::Flatpak)
}

#[cfg(target_os = "linux")]
fn find_on_path(name: &str) -> Option<PathBuf> {
    std::env::split_paths(&std::env::var_os("PATH")?).find(|dir| dir.join(name).is_file())
}

// ponytail: the Store install can't be seen without the WinRT package APIs, so Play is
// always offered and Windows reports a missing game itself
#[cfg(target_os = "windows")]
fn find_install(_home: Option<&directories::BaseDirs>) -> Option<Install> {
    Some(Install::Windows)
}

#[cfg(not(any(target_os = "macos", target_os = "linux", target_os = "windows")))]
fn find_install(_home: Option<&directories::BaseDirs>) -> Option<Install> {
    None
}

/// Folders that hold `versions` and `games/com.mojang`, most likely first
fn data_roots(home: Option<&directories::BaseDirs>, install: Option<&Install>) -> Vec<PathBuf> {
    let Some(home) = home else { return Vec::new() };
    let _ = install;
    #[cfg(target_os = "macos")]
    {
        vec![home.data_dir().join("mcpelauncher")]
    }
    #[cfg(target_os = "linux")]
    {
        let flatpak = home.home_dir().join(".var/app").join(FLATPAK_ID).join("data/mcpelauncher");
        let native = home.data_dir().join("mcpelauncher");
        match install {
            Some(Install::Flatpak) => vec![flatpak, native],
            _ => vec![native, flatpak],
        }
    }
    #[cfg(target_os = "windows")]
    {
        // GDK builds (1.21.120 and later) moved out of the UWP package folder
        vec![
            home.config_dir().join("Minecraft Bedrock/Users/Shared"),
            home.data_local_dir().join("Packages/Microsoft.MinecraftUWP_8wekyb3d8bbwe/LocalState"),
        ]
    }
    #[cfg(not(any(target_os = "macos", target_os = "linux", target_os = "windows")))]
    {
        Vec::new()
    }
}

fn newest_version(versions: &Path) -> Option<PathBuf> {
    installed_versions(versions).into_iter().next()
}

/// Game versions mcpelauncher has downloaded and extracted, newest first
fn installed_versions(versions: &Path) -> Vec<PathBuf> {
    let mut found: Vec<PathBuf> = std::fs::read_dir(versions)
        .map(|dir| dir.flatten().map(|entry| entry.path()).filter(|path| path.join("lib").is_dir()).collect())
        .unwrap_or_default();
    found.sort_by_key(|path| std::cmp::Reverse(version_key(path)));
    found
}

/// Names of the versions an instance can be set to, newest first
pub fn versions(found: &Bedrock) -> Vec<String> {
    let Some(root) = &found.root else { return Vec::new() };
    installed_versions(&root.join("versions"))
        .iter()
        .filter_map(|path| path.file_name().map(|n| n.to_string_lossy().into_owned()))
        .collect()
}

fn version_key(path: &Path) -> Vec<u32> {
    let name = path.file_name().and_then(|n| n.to_str()).unwrap_or_default();
    name.split('.').map(|part| part.parse().unwrap_or(0)).collect()
}

// What TwoClient installs itself. Bumping these is how an app update moves everyone
// to a newer mcpelauncher
#[cfg(all(target_os = "macos", target_arch = "aarch64"))]
const MAC_DMG_URL: &str =
    "https://github.com/minecraft-linux/macos-builder/releases/download/v1.8.5/Minecraft.Bedrock.Launcher.dmg";
#[cfg(all(target_os = "macos", target_arch = "aarch64"))]
const MAC_DMG_SHA256: &str = "1faa08ed77f998a4c049b597300525761c5543209de5e51199ab32aa611197f4";
#[cfg(all(target_os = "macos", target_arch = "aarch64"))]
const MAC_DMG_SIZE: u64 = 307_070_990;
#[cfg(target_os = "linux")]
const FLATHUB_REPO: &str = "https://dl.flathub.org/repo/flathub.flatpakrepo";

static INSTALLING: AtomicBool = AtomicBool::new(false);
static LAST_ERROR: Mutex<Option<String>> = Mutex::new(None);

/// Whether this platform gets an Install button. The macOS build is Apple silicon only
pub const fn can_install() -> bool {
    cfg!(any(all(target_os = "macos", target_arch = "aarch64"), target_os = "linux"))
}

pub fn is_installing() -> bool {
    INSTALLING.load(Ordering::Relaxed)
}

pub fn last_error() -> Option<String> {
    LAST_ERROR.lock().ok()?.clone()
}

/// Where TwoClient keeps its own copy, inside its data folder so a reset or uninstall
/// takes it along
#[cfg(target_os = "macos")]
fn managed_dir() -> Option<PathBuf> {
    twoclient_common::paths::data_dir().ok().map(|dir| dir.join("bedrock"))
}

/// Runs in the background so it survives leaving the page or finishing onboarding
pub fn start_install() {
    if !can_install() || INSTALLING.swap(true, Ordering::SeqCst) {
        return;
    }
    if let Ok(mut err) = LAST_ERROR.lock() {
        *err = None;
    }
    tokio::spawn(async {
        if let Err(err) = install().await {
            tracing::warn!("failed to install mcpelauncher: {err}");
            if let Ok(mut last) = LAST_ERROR.lock() {
                *last = Some(err);
            }
        }
        INSTALLING.store(false, Ordering::SeqCst);
    });
}

#[cfg(all(target_os = "macos", target_arch = "aarch64"))]
async fn install() -> Result<(), String> {
    let state = crate::launcher::state().map_err(|e| e.to_string())?;
    let dir = managed_dir().ok_or("no data folder")?;
    let dmg = dir.join("mcpelauncher.dmg");

    // reqwest writes no quarantine flag, so the app opens without the "damaged" prompt
    twoclient_net::download_verified(
        &state.services.requester,
        &state.services.events,
        MAC_DMG_URL,
        &dmg,
        Some(&polyio::Checksum::sha256(MAC_DMG_SHA256)),
        MAC_DMG_SIZE,
        Some(
            twoclient_net::ResponseNotifyOptions::standalone("Downloading Bedrock support")
                .done_label("Bedrock support installed"),
        ),
    )
    .await
    .map_err(|e| e.to_string())?;

    let result = tokio::task::spawn_blocking(move || copy_app_from_dmg(&dmg, &dir))
        .await
        .map_err(|e| e.to_string())?;
    result
}

#[cfg(all(target_os = "macos", target_arch = "aarch64"))]
fn copy_app_from_dmg(dmg: &Path, dir: &Path) -> Result<(), String> {
    let mount = dir.join("mount");
    let app = dir.join(MAC_APP);
    let _ = std::fs::remove_dir_all(&app);

    run(Command::new("hdiutil")
        .args(["attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint"])
        .arg(&mount)
        .arg(dmg))?;
    let copied = run(Command::new("ditto").arg(mount.join(MAC_APP)).arg(&app));
    let _ = run(Command::new("hdiutil").args(["detach", "-force"]).arg(&mount));
    let _ = std::fs::remove_file(dmg);
    copied
}

#[cfg(target_os = "linux")]
async fn install() -> Result<(), String> {
    tokio::task::spawn_blocking(|| {
        run(Command::new("flatpak")
            .args(["remote-add", "--user", "--if-not-exists", "flathub", FLATHUB_REPO]))?;
        run(Command::new("flatpak")
            .args(["install", "--user", "--noninteractive", "-y", "flathub", FLATPAK_ID]))
    })
    .await
    .map_err(|e| e.to_string())?
}

#[cfg(not(any(all(target_os = "macos", target_arch = "aarch64"), target_os = "linux")))]
async fn install() -> Result<(), String> {
    Err("not supported on this platform".to_string())
}

#[cfg(any(target_os = "macos", target_os = "linux"))]
fn run(cmd: &mut Command) -> Result<(), String> {
    let program = cmd.get_program().to_string_lossy().into_owned();
    let output = cmd.output().map_err(|e| format!("{program}: {e}"))?;
    if output.status.success() {
        return Ok(());
    }
    let stderr = String::from_utf8_lossy(&output.stderr);
    Err(format!("{program} failed: {}", stderr.trim()))
}

/// Starts the game itself, skipping mcpelauncher's own window when a version is ready
pub fn play(found: &Bedrock) {
    let Some(install) = &found.install else { return };
    let result = match (install, &found.version) {
        #[cfg(target_os = "windows")]
        (Install::Windows, _) => open::that_detached("minecraft:"),
        #[cfg(not(target_os = "windows"))]
        (install, Some(version)) => {
            let mut cmd = client_command(install);
            cmd.arg("-dg").arg(version);
            if let Some(mods) = bundled_mods_dir() {
                cmd.arg("-m").arg(mods);
            }
            cmd.spawn().map(drop)
        }
        #[cfg(not(target_os = "windows"))]
        (install, None) => return open_launcher(install),
    };
    if let Err(err) = result {
        tracing::warn!("failed to start Bedrock: {err}");
    }
}

/// The Xbox sign-in opens in mcpelauncher's embedded Chromium, which claims passkey
/// support it can't deliver, so Microsoft keeps offering a passkey prompt that never
/// opens. Without WebAuthn it falls back to the password page.
#[cfg(not(target_os = "windows"))]
const WEBVIEW_FLAGS: &str = "--disable-blink-features=WebAuth";

#[cfg(not(target_os = "windows"))]
fn client_command(install: &Install) -> Command {
    match install {
        #[cfg(target_os = "macos")]
        Install::MacApp(app) => {
            let mut cmd = Command::new(app.join("Contents/MacOS/mcpelauncher-client"));
            cmd.env("QTWEBENGINE_CHROMIUM_FLAGS", WEBVIEW_FLAGS);
            cmd
        }
        #[cfg(target_os = "linux")]
        Install::Binary(dir) => {
            let mut cmd = Command::new(dir.join("mcpelauncher-client"));
            cmd.env("QTWEBENGINE_CHROMIUM_FLAGS", WEBVIEW_FLAGS);
            cmd
        }
        #[cfg(target_os = "linux")]
        Install::Flatpak => {
            let mut cmd = Command::new("flatpak");
            cmd.args(["run", "--command=mcpelauncher-client"]);
            cmd.arg(format!("--env=QTWEBENGINE_CHROMIUM_FLAGS={WEBVIEW_FLAGS}"));
            // the sandbox only sees its own folder unless instances are shared with it
            if let Some(dir) = instances_dir() {
                cmd.arg(format!("--filesystem={}", dir.display()));
            }
            cmd.arg(FLATPAK_ID);
            cmd
        }
    }
}

// --- instances ---------------------------------------------------------------
//
// An instance is a folder of its own that mcpelauncher keeps the game's data in
// (`-dd`): worlds, packs, options and its settings. The game files themselves stay
// in mcpelauncher's `versions` and are shared. Windows has one install and no
// instances.

#[derive(Clone, Debug, PartialEq)]
pub struct Instance {
    pub dir: PathBuf,
    pub name: String,
    /// The game version it plays, newest installed when unset
    pub version: Option<String>,
    /// Unix seconds
    pub last_played: Option<i64>,
}

const INSTANCE_FILE: &str = "instance.txt";

impl Instance {
    /// What mcpelauncher gets as `-dd`
    pub fn data_root(&self) -> PathBuf {
        self.dir.join("data")
    }

    /// The instance's `com.mojang`, where packs and worlds go
    pub fn data_dir(&self) -> PathBuf {
        self.data_root().join("games/com.mojang")
    }

    fn save(&self) -> std::io::Result<()> {
        let mut text = format!("name={}\n", self.name.replace('\n', " "));
        if let Some(version) = &self.version {
            text.push_str(&format!("version={version}\n"));
        }
        if let Some(when) = self.last_played {
            text.push_str(&format!("last_played={when}\n"));
        }
        std::fs::write(self.dir.join(INSTANCE_FILE), text)
    }

    fn load(dir: PathBuf) -> Option<Self> {
        let text = std::fs::read_to_string(dir.join(INSTANCE_FILE)).ok()?;
        let field = |key: &str| {
            text.lines().filter_map(|l| l.split_once('=')).find(|(k, _)| *k == key).map(|(_, v)| v.trim().to_string())
        };
        Some(Instance {
            name: field("name")?,
            version: field("version").filter(|v| !v.is_empty()),
            last_played: field("last_played").and_then(|v| v.parse().ok()),
            dir,
        })
    }
}

pub fn instances_dir() -> Option<PathBuf> {
    twoclient_common::paths::data_dir().ok().map(|dir| dir.join("bedrock/instances"))
}

/// Every instance, most recently played first
pub fn instances() -> Vec<Instance> {
    let Some(dir) = instances_dir() else { return Vec::new() };
    let mut found: Vec<Instance> = std::fs::read_dir(dir)
        .map(|entries| entries.flatten().filter_map(|e| Instance::load(e.path())).collect())
        .unwrap_or_default();
    found.sort_by(|a, b| b.last_played.cmp(&a.last_played).then_with(|| a.name.cmp(&b.name)));
    found
}

/// A folder name from the instance name, made unique among the others
fn folder_for(parent: &Path, name: &str) -> PathBuf {
    let base: String = name
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() || c == '-' || c == '_' { c } else { '-' })
        .collect::<String>()
        .trim_matches('-')
        .to_lowercase();
    let base = if base.is_empty() { "instance".to_string() } else { base };
    let mut candidate = parent.join(&base);
    let mut n = 2;
    while candidate.exists() {
        candidate = parent.join(format!("{base}-{n}"));
        n += 1;
    }
    candidate
}

pub fn create_instance(name: &str, version: Option<String>) -> std::io::Result<Instance> {
    let parent = instances_dir().ok_or_else(|| std::io::Error::other("no data folder"))?;
    std::fs::create_dir_all(&parent)?;
    let name = name.trim();
    let instance = Instance {
        dir: folder_for(&parent, name),
        name: if name.is_empty() { "Bedrock".to_string() } else { name.to_string() },
        version,
        last_played: None,
    };
    std::fs::create_dir_all(instance.data_dir())?;
    instance.save()?;
    Ok(instance)
}

pub fn set_instance_version(instance: &Instance, version: Option<String>) -> std::io::Result<()> {
    Instance { version, ..instance.clone() }.save()
}

/// Removes the instance's folder and every world in it
pub fn delete_instance(instance: &Instance) -> std::io::Result<()> {
    // only ever a folder TwoClient made, inside its instances folder
    let parent = instances_dir().ok_or_else(|| std::io::Error::other("no data folder"))?;
    if instance.dir.parent() != Some(parent.as_path()) || !instance.dir.join(INSTANCE_FILE).is_file() {
        return Err(std::io::Error::other("not an instance folder"));
    }
    std::fs::remove_dir_all(&instance.dir)
}

/// Windows has one install and keeps its own data, so an instance just starts the game
#[cfg(target_os = "windows")]
pub fn play_instance(found: &Bedrock, _instance: &Instance) {
    play(found);
}

/// Starts the game on an instance's data, with its version if that is still installed
#[cfg(not(target_os = "windows"))]
pub fn play_instance(found: &Bedrock, instance: &Instance) {
    let (Some(install), Some(root)) = (&found.install, &found.root) else { return };
    let version = instance
        .version
        .as_ref()
        .map(|v| root.join("versions").join(v))
        .filter(|p| p.join("lib").is_dir())
        .or_else(|| found.version.clone());
    let Some(version) = version else { return open_launcher(install) };
    let _ = std::fs::create_dir_all(instance.data_dir());
    share_sign_in(root, instance);
    let mut cmd = client_command(install);
    cmd.arg("-dg").arg(version).arg("-dd").arg(instance.data_root());
    if let Some(mods) = bundled_mods_dir() {
        cmd.arg("-m").arg(mods);
    }
    match cmd.spawn() {
        Ok(_) => {
            let now = chrono::Utc::now().timestamp();
            let _ = Instance { last_played: Some(now), ..instance.clone() }.save();
        }
        Err(err) => tracing::warn!("failed to start Bedrock: {err}"),
    }
}

/// The game keeps its Xbox sign-in in `xal` next to `games`. Pointing every instance
/// at mcpelauncher's own copy signs them all in at once, and a refreshed sign-in
/// carries over to the rest
#[cfg(unix)]
fn share_sign_in(root: &Path, instance: &Instance) {
    let shared = root.join("xal");
    let link = instance.data_root().join("xal");
    if link.symlink_metadata().is_ok() {
        return;
    }
    let _ = std::fs::create_dir_all(&shared);
    if let Err(err) = std::os::unix::fs::symlink(&shared, &link) {
        tracing::warn!("couldn't share the Xbox sign-in with the instance: {err}");
    }
}

/// The in-game mod, built into the app by build.rs; empty when this build has none
#[cfg(not(target_os = "windows"))]
const BUNDLED_MOD: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/bedrock_mod.so"));

/// Where TwoClient keeps its own Bedrock mod, written out from the app so an app
/// update brings the mod along, and loaded into every game it starts
#[cfg(not(target_os = "windows"))]
fn bundled_mods_dir() -> Option<PathBuf> {
    let dir = twoclient_common::paths::data_dir().ok()?.join("bedrock/mods");
    if !BUNDLED_MOD.is_empty() {
        let file = dir.join("libtwoclient_bedrock.so");
        let current = std::fs::read(&file).ok();
        if current.as_deref() != Some(BUNDLED_MOD) {
            let written = std::fs::create_dir_all(&dir).and_then(|_| std::fs::write(&file, BUNDLED_MOD));
            if let Err(err) = written {
                tracing::warn!("couldn't put the Bedrock mod in place: {err}");
            }
        }
    }
    std::fs::read_dir(&dir).ok()?.flatten().any(|e| e.path().extension().is_some_and(|x| x == "so")).then_some(dir)
}

/// mcpelauncher's own window, where the game is signed in to and downloaded
pub fn open_launcher(install: &Install) {
    let result = match install {
        #[cfg(target_os = "macos")]
        Install::MacApp(app) => Command::new("open").arg(app).spawn().map(drop),
        #[cfg(target_os = "linux")]
        Install::Binary(dir) => Command::new(dir.join("mcpelauncher-ui-qt")).spawn().map(drop),
        #[cfg(target_os = "linux")]
        Install::Flatpak => Command::new("flatpak").args(["run", FLATPAK_ID]).spawn().map(drop),
        #[cfg(target_os = "windows")]
        Install::Windows => open::that_detached("minecraft:"),
    };
    if let Err(err) = result {
        tracing::warn!("failed to open mcpelauncher: {err}");
    }
}

/// mcpelauncher's known issue: Bedrock 26.50 and newer draws wrong on M3 chips
#[cfg(target_os = "macos")]
pub fn chip_warning() -> Option<&'static str> {
    let out = Command::new("sysctl").args(["-n", "machdep.cpu.brand_string"]).output().ok()?;
    let chip = String::from_utf8_lossy(&out.stdout);
    chip.contains("M3").then_some(
        "M3 Macs show graphical glitches on Bedrock 26.50 and newer. Pick an older version in Manage versions.",
    )
}

#[cfg(not(target_os = "macos"))]
pub fn chip_warning() -> Option<&'static str> {
    None
}

pub fn status(found: &Bedrock) -> String {
    match (&found.install, &found.version) {
        #[cfg(target_os = "windows")]
        (Some(_), _) => "Minecraft for Windows".to_string(),
        (Some(_), Some(version)) => {
            let name = version.file_name().unwrap_or_default().to_string_lossy();
            format!("Bedrock {name}")
        }
        (Some(_), None) => "mcpelauncher found, no game downloaded yet".to_string(),
        (None, _) => "mcpelauncher isn't installed".to_string(),
    }
}

/// Graphics options traded for frames. View distance is in blocks, 128 is 8 chunks
const PERFORMANCE_OPTIONS: &[(&str, &str)] = &[
    ("gfx_viewdistance", "128"),
    ("gfx_fancyskies", "0"),
    ("gfx_transparentleaves", "0"),
    ("gfx_smoothlighting", "0"),
];
const BACKUP_FILE: &str = "options.twoclient-backup.txt";

fn options_file(data_dir: &Path) -> PathBuf {
    data_dir.join("minecraftpe/options.txt")
}

/// The game's own "sprint on movement", which keeps sprinting while you walk, the
/// way a toggle sprint mod does on Java
const AUTO_SPRINT_KEY: &str = "ctrl_sprintonmovement";

pub fn toggle_sprint(data_dir: &Path) -> bool {
    std::fs::read_to_string(options_file(data_dir))
        .ok()
        .and_then(|text| text.lines().find_map(|l| l.strip_prefix(AUTO_SPRINT_KEY)?.strip_prefix(':').map(|v| v.trim() == "1")))
        .unwrap_or(false)
}

/// Sets it for the game's own data and every instance, so it follows you around
pub fn set_toggle_sprint(data_dir: &Path, on: bool) -> std::io::Result<()> {
    let value = if on { "1" } else { "0" };
    let mut dirs = vec![data_dir.to_path_buf()];
    dirs.extend(instances().iter().map(Instance::data_dir));
    for dir in dirs {
        let file = options_file(&dir);
        // an instance that hasn't run yet has no options file; the game makes one around it
        let text = std::fs::read_to_string(&file).unwrap_or_default();
        let (updated, _) = apply_options(&text, &[(AUTO_SPRINT_KEY, value)], ':');
        if let Some(parent) = file.parent() {
            std::fs::create_dir_all(parent)?;
        }
        std::fs::write(file, updated)?;
    }
    Ok(())
}

/// On while the backup of the player's own values exists
pub fn performance_mode(data_dir: &Path) -> bool {
    data_dir.join("minecraftpe").join(BACKUP_FILE).is_file()
}

/// The game rewrites options.txt when it closes, so this takes effect on the next launch
pub fn set_performance_mode(data_dir: &Path, on: bool) -> std::io::Result<()> {
    let options = options_file(data_dir);
    let backup = data_dir.join("minecraftpe").join(BACKUP_FILE);
    let text = std::fs::read_to_string(&options)?;
    if on {
        let (tuned, saved) = apply_options(&text, PERFORMANCE_OPTIONS, ':');
        std::fs::write(&backup, saved)?;
        std::fs::write(&options, tuned)
    } else {
        let saved = std::fs::read_to_string(&backup)?;
        let pairs: Vec<(&str, &str)> = saved.lines().filter_map(|l| l.split_once(':')).collect();
        let (restored, _) = apply_options(&text, &pairs, ':');
        std::fs::write(&options, restored)?;
        std::fs::remove_file(&backup)
    }
}

/// Returns the new text and the replaced lines, so applying those undoes it
fn apply_options(text: &str, pairs: &[(&str, &str)], sep: char) -> (String, String) {
    let mut saved = String::new();
    let mut out: Vec<String> = text
        .lines()
        .map(|line| {
            let key = line.split_once(sep).map(|(k, _)| k);
            match pairs.iter().find(|(k, _)| Some(*k) == key) {
                Some((k, v)) => {
                    saved.push_str(line);
                    saved.push('\n');
                    format!("{k}{sep}{v}")
                }
                None => line.to_string(),
            }
        })
        .collect();
    // Keys the game hasn't written yet still get set
    for (k, v) in pairs {
        if !text.lines().any(|l| l.split_once(sep).map(|(key, _)| key) == Some(*k)) {
            out.push(format!("{k}{sep}{v}"));
        }
    }
    let mut joined = out.join("\n");
    if text.ends_with('\n') {
        joined.push('\n');
    }
    (joined, saved)
}

/// VSync and the FPS counter live in mcpelauncher's own settings, next to `games`.
/// On Windows VSync is the game's option and there is no FPS counter
fn setting_file(data_dir: &Path) -> (PathBuf, char) {
    if cfg!(target_os = "windows") {
        (options_file(data_dir), ':')
    } else {
        let root = data_dir.parent().and_then(Path::parent).unwrap_or(data_dir);
        (root.join("mcpelauncher-client-settings.txt"), '=')
    }
}

fn read_setting(data_dir: &Path, key: &str) -> Option<String> {
    let (file, sep) = setting_file(data_dir);
    let text = std::fs::read_to_string(file).ok()?;
    text.lines()
        .filter_map(|l| l.split_once(sep))
        .find(|(k, _)| *k == key)
        .map(|(_, v)| v.trim().to_string())
}

fn write_setting(data_dir: &Path, key: &str, value: &str) -> std::io::Result<()> {
    let (file, sep) = setting_file(data_dir);
    let text = std::fs::read_to_string(&file).unwrap_or_default();
    let (updated, _) = apply_options(&text, &[(key, value)], sep);
    std::fs::write(file, updated)
}

const VSYNC_KEY: &str = if cfg!(target_os = "windows") { "gfx_vsync" } else { "vsync" };

pub fn vsync(data_dir: &Path) -> bool {
    // mcpelauncher and the game both default to on
    read_setting(data_dir, VSYNC_KEY).is_none_or(|v| v == "1" || v == "true")
}

pub fn set_vsync(data_dir: &Path, on: bool) -> std::io::Result<()> {
    let value = match (cfg!(target_os = "windows"), on) {
        (true, on) => if on { "1" } else { "0" },
        (false, on) => if on { "true" } else { "false" },
    };
    write_setting(data_dir, VSYNC_KEY, value)
}

pub const HAS_FPS_COUNTER: bool = !cfg!(target_os = "windows");

pub fn fps_counter(data_dir: &Path) -> bool {
    read_setting(data_dir, "enable_fps_hud").is_some_and(|v| v != "0")
}

/// 2 is mcpelauncher's "Ingame", so the counter stays off the menus
pub fn set_fps_counter(data_dir: &Path, on: bool) -> std::io::Result<()> {
    write_setting(data_dir, "enable_fps_hud", if on { "2" } else { "0" })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn newest_version_compares_numerically() {
        let dir = std::env::temp_dir().join(format!("twoclient-bedrock-{}", std::process::id()));
        for name in ["1.21.9.1", "1.21.114.1", "1.20.80.5"] {
            std::fs::create_dir_all(dir.join(name).join("lib")).unwrap();
        }
        // half-extracted downloads have no lib folder and must be skipped
        std::fs::create_dir_all(dir.join("1.99.0.0")).unwrap();

        let newest = newest_version(&dir).unwrap();
        std::fs::remove_dir_all(&dir).unwrap();
        assert!(newest.ends_with("1.21.114.1"));
    }

    #[test]
    fn performance_options_round_trip() {
        let original = "gfx_viewdistance:240\nmp_username:tuna\ngfx_vsync:1\n";
        let (tuned, saved) = apply_options(original, PERFORMANCE_OPTIONS, ':');
        assert!(tuned.contains("gfx_viewdistance:128\n"));
        assert!(tuned.contains("mp_username:tuna\n"));

        let pairs: Vec<(&str, &str)> = saved.lines().filter_map(|l| l.split_once(':')).collect();
        let (restored, _) = apply_options(&tuned, &pairs, ':');
        // keys the player never had stay tuned, the rest go back
        assert!(restored.starts_with(original));
    }
}
