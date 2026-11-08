//! Which edition the launcher is showing. Java and Bedrock share the same pages;
//! each page asks this which game to show.

use std::path::PathBuf;

use freya::animation::*;
use freya::prelude::*;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Edition {
    Java,
    Bedrock,
}

impl Edition {
    pub const ALL: [Edition; 2] = [Edition::Java, Edition::Bedrock];

    pub fn label(self) -> &'static str {
        match self {
            Edition::Java => "Java",
            Edition::Bedrock => "Bedrock",
        }
    }

    pub fn title(self) -> &'static str {
        match self {
            Edition::Java => "JAVA EDITION",
            Edition::Bedrock => "BEDROCK EDITION",
        }
    }
}

fn file() -> Option<PathBuf> {
    twoclient_common::paths::data_dir().ok().map(|dir| dir.join("edition.txt"))
}

fn load() -> Edition {
    match file().and_then(|f| std::fs::read_to_string(f).ok()).as_deref().map(str::trim) {
        Some("bedrock") => Edition::Bedrock,
        _ => Edition::Java,
    }
}

fn save(edition: Edition) {
    let text = if edition == Edition::Bedrock { "bedrock" } else { "java" };
    if let Some(f) = file()
        && let Err(err) = std::fs::write(f, text)
    {
        tracing::warn!("failed to remember the edition: {err}");
    }
}

#[derive(Clone, Copy)]
pub struct EditionState {
    pub current: State<Edition>,
    /// The edition a running switch animation is heading to
    pub switching: State<Option<Edition>>,
}

impl EditionState {
    /// Starts the switch; the transition overlay flips `current` once the screen is covered
    pub fn switch_to(mut self, edition: Edition) {
        if *self.current.peek() == edition || self.switching.peek().is_some() {
            return;
        }
        self.switching.set(Some(edition));
        play_switch_sound(edition);
    }

    pub fn commit(mut self, edition: Edition) {
        self.current.set(edition);
        save(edition);
    }
}

pub fn use_provide_edition() {
    let current = use_state(load);
    let switching = use_state(|| None::<Edition>);
    use_provide_root_context(move || EditionState { current, switching });
}

pub fn use_edition() -> EditionState {
    consume_root_context::<EditionState>()
}

// Each edition has its own: a bright chime into Bedrock, a woody pluck into Java
const TO_BEDROCK_SOUND: &[u8] = include_bytes!("../assets/sounds/edition-switch-bedrock.wav");
const TO_JAVA_SOUND: &[u8] = include_bytes!("../assets/sounds/edition-switch-java.wav");

/// Plays the sound for switching to `edition` without blocking, through what each
/// system already has
fn play_switch_sound(edition: Edition) {
    let (sound, name) = match edition {
        Edition::Bedrock => (TO_BEDROCK_SOUND, "twoclient-to-bedrock.wav"),
        Edition::Java => (TO_JAVA_SOUND, "twoclient-to-java.wav"),
    };
    #[cfg(windows)]
    {
        use windows::Win32::Media::Audio::{PlaySoundA, SND_ASYNC, SND_MEMORY, SND_NODEFAULT};
        use windows::core::PCSTR;
        // SND_MEMORY reads the WAV straight from the binary, which lives for the whole run
        unsafe {
            let _ = PlaySoundA(PCSTR(sound.as_ptr()), None, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
        }
    }
    #[cfg(not(windows))]
    {
        let path = std::env::temp_dir().join(name);
        if !path.is_file() && std::fs::write(&path, sound).is_err() {
            return;
        }
        let players: &[(&str, &[&str])] = if cfg!(target_os = "macos") {
            &[("afplay", &[])]
        } else {
            &[("paplay", &[]), ("pw-play", &[]), ("aplay", &["-q"])]
        };
        for (program, args) in players {
            if std::process::Command::new(program).args(*args).arg(&path).spawn().is_ok() {
                return;
            }
        }
    }
}

// --- the switch animation ------------------------------------------------------
//
// Blocks fill the window in a diagonal sweep, the new edition's name lands in
// the middle, and the blocks clear the same way they came. The timing follows
// the sound: covered at the thump, the name on the chime.

const SWITCH_MS: u64 = 1500;
const COVERED_MS: u64 = 620;
const BLOCK_PX: f32 = 72.;

/// 0..1 of the way through `t` between `start` and `end`, eased
fn phase(t: f32, start: f32, end: f32) -> f32 {
    ((t - start) / (end - start)).clamp(0., 1.)
}

fn ease_out_back(x: f32) -> f32 {
    let c1 = 1.70158;
    let c3 = c1 + 1.;
    1. + c3 * (x - 1.).powi(3) + c1 * (x - 1.).powi(2)
}

fn ease_in_cubic(x: f32) -> f32 {
    x * x * x
}

#[derive(PartialEq)]
pub struct EditionTransition;

impl Component for EditionTransition {
    fn render(&self) -> impl IntoElement {
        let state = use_edition();
        let target = *state.switching.read();

        let anim = use_animation_with_dependencies(&target, |conf, target| {
            if target.is_some() {
                conf.on_change(OnChange::Rerun);
            }
            AnimNum::new(0., 1.).time(SWITCH_MS).function(Function::Linear)
        });

        // flip the edition once the window is covered, and end the switch after
        use_side_effect_with_deps(&target, move |target| {
            let Some(edition) = *target else { return };
            let mut switching = state.switching;
            spawn(async move {
                tokio::time::sleep(std::time::Duration::from_millis(COVERED_MS)).await;
                state.commit(edition);
                let _ = freya::router::RouterContext::get().push(crate::Route::Home {});
                tokio::time::sleep(std::time::Duration::from_millis(SWITCH_MS - COVERED_MS)).await;
                switching.set(None);
            });
        });

        let Some(edition) = target else {
            return rect().into_element();
        };

        let t = anim.get().value() as f32;
        let size = *Platform::get().root_size.read();
        let (w, h) = (size.width.max(1.), size.height.max(1.));
        let cols = (w / BLOCK_PX).ceil() as i32 + 1;
        let rows = (h / BLOCK_PX).ceil() as i32 + 1;
        let span = (cols + rows) as f32;

        // the target edition's blocks: grassy dark for Java, stone dark for Bedrock
        let (base, accent) = match edition {
            Edition::Java => ((22, 30, 20), (30, 40, 26)),
            Edition::Bedrock => ((22, 25, 30), (30, 34, 41)),
        };

        let mut layer = rect()
            .width(Size::fill())
            .height(Size::fill())
            .position(Position::new_global().top(0.).left(0.))
            .layer(Layer::Overlay);

        let covered = COVERED_MS as f32 / SWITCH_MS as f32;
        for row in 0..rows {
            for col in 0..cols {
                // each block starts a little after the one up and to the left of it
                let order = (col + row) as f32 / span;
                let grow = phase(t, order * 0.24, order * 0.24 + covered - 0.24);
                let shrink = phase(t, 0.68 + order * 0.2, 0.68 + order * 0.2 + 0.12);
                let k = (ease_out_back(grow) * (1. - ease_in_cubic(shrink))).max(0.);
                if k <= 0.001 {
                    continue;
                }
                // a little noise in the shade so it reads as blocks, not a wipe
                let noise = (col.wrapping_mul(73856093) ^ row.wrapping_mul(19349663)).rem_euclid(5) as f32 / 4.;
                let mix = |a: u8, b: u8| (a as f32 + (b as f32 - a as f32) * noise) as u8;
                let color = Color::from_rgb(mix(base.0, accent.0), mix(base.1, accent.1), mix(base.2, accent.2));
                let s = BLOCK_PX * k.min(1.06);
                let x = col as f32 * BLOCK_PX + (BLOCK_PX - s) / 2.;
                let y = row as f32 * BLOCK_PX + (BLOCK_PX - s) / 2.;
                layer = layer.child(
                    rect()
                        .position(Position::new_absolute().left(x).top(y))
                        .width(Size::px(s + 1.))
                        .height(Size::px(s + 1.))
                        .corner_radius(CornerRadius::new_all(3. * (1. - k.min(1.)) + 1.))
                        .background(color),
                );
            }
        }

        // the name lands on the chime and leaves as the blocks clear
        let show = phase(t, 0.44, 0.6);
        let hide = phase(t, 0.7, 0.8);
        let alpha = (show * (1. - hide)).clamp(0., 1.);
        if alpha > 0. {
            let pop = ease_out_back(show);
            layer = layer.child(
                rect()
                    .width(Size::fill())
                    .height(Size::fill())
                    .position(Position::new_absolute().top(0.).left(0.))
                    .center()
                    .opacity(alpha)
                    .scale(0.86 + 0.14 * pop)
                    .child(
                        rect()
                            .vertical()
                            .cross_align(Alignment::Center)
                            .spacing(6.)
                            .child(
                                label()
                                    .text("MINECRAFT")
                                    .font_size(64.)
                                    .font_weight(FontWeight::BOLD)
                                    .color(Color::WHITE),
                            )
                            .child(
                                label()
                                    .text(edition.title())
                                    .font_size(18.)
                                    .font_weight(FontWeight::MEDIUM)
                                    .color(crate::theme::colors::brand()),
                            ),
                    ),
            );
        }
        layer.into_element()
    }
}
