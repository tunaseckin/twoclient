use std::time::Duration;

use freya::animation::*;
use freya::prelude::*;

use crate::bedrock::{self, Bedrock as Detected};
use crate::components::{Button, Dropdown, IconType, TextInput, toggle_controlled};
use crate::platform;
use crate::theme::colors;
use crate::ui::border_all_color;
use crate::view::app::settings::settings_row;

#[derive(PartialEq)]
pub struct Bedrock;

impl Component for Bedrock {
    fn render(&self) -> impl IntoElement {
        let mut detected = use_state(bedrock::detect);
        let mut installing = use_state(bedrock::is_installing);
        let found = detected.read().clone();

        // The install runs on its own task, so watch it from here until it lands
        let mut watching = use_state(|| false);
        if *installing.read() && !*watching.peek() {
            watching.set(true);
            spawn(async move {
                while bedrock::is_installing() {
                    tokio::time::sleep(Duration::from_millis(500)).await;
                }
                detected.set(bedrock::detect());
                installing.set(false);
                watching.set(false);
            });
        }

        let start = move || {
            bedrock::start_install();
            installing.set(true);
        };

        let page = rect()
            .vertical()
            .width(Size::fill())
            .padding(Gaps::new(0., 40., 40., 40.))
            .spacing(24.)
            .child(header())
            .child(game_card(
                &found,
                *installing.read(),
                start,
                move || detected.set(bedrock::detect()),
            ))
            .maybe_child(found.data_dir.clone().map(|dir| GameSettings { dir }.into_element()))
            .maybe_child(
                (!cfg!(target_os = "windows") && found.install.is_some() && found.version.is_some())
                    .then(|| Instances { found: found.clone() }.into_element()),
            );

        rect()
            .width(Size::fill())
            .height(Size::fill())
            .child(ScrollView::new().child(page))
    }
}

/// Instances, each its own set of worlds, packs and settings on a chosen version
#[derive(PartialEq)]
struct Instances {
    found: Detected,
}

impl Component for Instances {
    fn render(&self) -> impl IntoElement {
        let mut list = use_state(bedrock::instances);
        let mut name = use_state(String::new);
        // the instance whose Delete was pressed once and now asks to be pressed again
        let mut confirming = use_state(|| None::<std::path::PathBuf>);
        let versions = bedrock::versions(&self.found);

        // The field can still be holding its value when it submits, so the read is
        // copied out and the writes wait for the next tick
        let create = move || {
            let typed = name.peek().clone();
            spawn(async move {
                match bedrock::create_instance(&typed, None) {
                    Ok(_) => {
                        name.set(String::new());
                        list.set(bedrock::instances());
                    }
                    Err(err) => tracing::warn!("failed to create a Bedrock instance: {err}"),
                }
            });
        };

        let mut rows = rect().vertical().width(Size::fill()).spacing(8.);
        for instance in list.read().iter().cloned() {
            rows = rows.child(instance_row(
                instance,
                &self.found,
                &versions,
                confirming.read().clone(),
                move || list.set(bedrock::instances()),
                move |dir| confirming.set(dir),
            ));
        }

        rect()
            .vertical()
            .width(Size::fill())
            .spacing(12.)
            .child(
                label()
                    .text("Instances")
                    .font_size(18.)
                    .font_weight(FontWeight::BOLD)
                    .color(colors::fg_primary()),
            )
            .child(
                label()
                    .text("Each instance keeps its own worlds, packs and settings, and shares your Xbox sign-in.")
                    .font_size(12.)
                    .color(colors::fg_secondary()),
            )
            .child(
                rect()
                    .horizontal()
                    .spacing(8.)
                    .cross_align(Alignment::Center)
                    .child(
                        rect()
                            .width(Size::px(280.))
                            .child(TextInput::new(name).placeholder("New instance name").on_submit(move |_| create())),
                    )
                    .child(Button::new().primary().on_press(move |_| create()).text("Create")),
            )
            .child(rows)
    }
}

fn instance_row(
    instance: bedrock::Instance,
    found: &Detected,
    versions: &[String],
    confirming: Option<std::path::PathBuf>,
    mut refresh: impl FnMut() + Clone + 'static,
    mut confirm: impl FnMut(Option<std::path::PathBuf>) + Clone + 'static,
) -> Element {
    let version_label = instance.version.clone().unwrap_or_else(|| "Newest installed".to_string());
    let mut options = vec!["Newest installed".to_string()];
    options.extend(versions.iter().cloned());

    let detail = match instance.last_played {
        Some(when) => chrono::DateTime::from_timestamp(when, 0)
            .map(|t| format!("Last played {}", t.with_timezone(&chrono::Local).format("%b %-d, %H:%M")))
            .unwrap_or_default(),
        None => "Never played".to_string(),
    };

    let to_play = (found.clone(), instance.clone());
    let to_open = instance.data_dir();
    let to_set = instance.clone();
    let to_delete = instance.clone();
    let versions = versions.to_vec();
    let asking = confirming.as_ref() == Some(&instance.dir);
    let mut refresh_play = refresh.clone();
    let mut refresh_set = refresh.clone();
    let mut confirm_ask = confirm.clone();

    rect()
        .horizontal()
        .content(Content::Flex)
        .cross_align(Alignment::Center)
        .width(Size::fill())
        .padding(16.)
        .spacing(8.)
        .corner_radius(CornerRadius::new_all(12.))
        .background(colors::component_bg())
        .border(border_all_color(1., colors::component_border()))
        .child(
            rect()
                .vertical()
                .width(Size::flex(1.))
                .spacing(2.)
                .child(label().text(instance.name.clone()).font_size(14.).color(colors::fg_primary()))
                .child(label().text(detail).font_size(12.).color(colors::fg_secondary())),
        )
        .child(Dropdown::new(version_label, options).width(Size::px(180.)).height(Size::px(34.)).on_select(
            move |idx: usize| {
                let version = idx.checked_sub(1).and_then(|i| versions.get(i).cloned());
                if let Err(err) = bedrock::set_instance_version(&to_set, version) {
                    tracing::warn!("failed to change the instance's version: {err}");
                }
                refresh_set();
            },
        ))
        .child(Button::new().primary().on_press(move |_| {
            bedrock::play_instance(&to_play.0, &to_play.1);
            refresh_play();
        })
        .text("Play"))
        .child(
            Button::new()
                .secondary()
                .on_press(move |_| {
                    let _ = std::fs::create_dir_all(&to_open);
                    platform::open_path(&to_open.to_string_lossy());
                })
                .text("Open folder"),
        )
        .child(
            Button::new()
                .secondary()
                .on_press(move |_| {
                    if !asking {
                        confirm_ask(Some(to_delete.dir.clone()));
                        return;
                    }
                    if let Err(err) = bedrock::delete_instance(&to_delete) {
                        tracing::warn!("failed to delete the instance: {err}");
                    }
                    confirm(None);
                    refresh();
                })
                .text(if asking { "Delete worlds too?" } else { "Delete" }),
        )
        .into_element()
}

/// Only shown once the game has run, since that is when its settings files exist
#[derive(PartialEq)]
struct GameSettings {
    dir: std::path::PathBuf,
}

impl Component for GameSettings {
    fn render(&self) -> impl IntoElement {
        game_settings(self.dir.clone())
    }
}

fn game_settings(dir: std::path::PathBuf) -> impl IntoElement {
    let performance = use_state({
        let dir = dir.clone();
        move || bedrock::performance_mode(&dir)
    });
    let vsync = use_state({
        let dir = dir.clone();
        move || bedrock::vsync(&dir)
    });
    let fps = use_state({
        let dir = dir.clone();
        move || bedrock::fps_counter(&dir)
    });
    let sprint = use_state({
        let dir = dir.clone();
        move || bedrock::toggle_sprint(&dir)
    });

    let mut rows = rect()
        .vertical()
        .width(Size::fill())
        .spacing(8.)
        .child(settings_row(
            IconType::Rocket02,
            "Performance mode",
            "Lowers view distance and turns off fancy skies, transparent leaves and smooth lighting. Turning it off puts your settings back.",
            setting_toggle(performance, dir.clone(), bedrock::set_performance_mode),
        ))
        .child(settings_row(
            IconType::RefreshCw01,
            "VSync",
            "Caps the frame rate to your display. Turn it off for more FPS.",
            setting_toggle(vsync, dir.clone(), bedrock::set_vsync),
        ))
        .child(settings_row(
            IconType::ChevronsRight,
            "Toggle sprint",
            "Keeps you sprinting while you walk, for this game and every instance.",
            setting_toggle(sprint, dir.clone(), bedrock::set_toggle_sprint),
        ));

    if bedrock::HAS_FPS_COUNTER {
        rows = rows.child(settings_row(
            IconType::Eye,
            "Show FPS",
            "Shows a frame counter while you play.",
            setting_toggle(fps, dir, bedrock::set_fps_counter),
        ));
    }

    rect()
        .vertical()
        .width(Size::fill())
        .spacing(12.)
        .child(
            label()
                .text("Changes apply the next time you start the game.")
                .font_size(12.)
                .color(colors::fg_secondary()),
        )
        .child(rows)
}

fn setting_toggle(
    mut value: State<bool>,
    dir: std::path::PathBuf,
    apply: fn(&std::path::Path, bool) -> std::io::Result<()>,
) -> impl IntoElement {
    let on = *value.read();
    toggle_controlled(
        on,
        EventHandler::new(move |()| match apply(&dir, !on) {
            Ok(()) => value.set(!on),
            Err(err) => tracing::warn!("failed to change a Bedrock setting: {err}"),
        }),
    )
}

fn header() -> Element {
    rect()
        .vertical()
        .spacing(2.)
        .child(
            label()
                .text("Bedrock")
                .font_size(32.)
                .font_weight(FontWeight::BOLD)
                .color(colors::fg_primary()),
        )
        .child(
            label()
                .text(subtitle())
                .font_size(13.)
                .color(colors::fg_secondary()),
        )
        .maybe_child((!cfg!(target_os = "windows")).then(google_play_note))
        .into_element()
}

const GOOGLE_PLAY_URL: &str = "https://play.google.com/store/apps/details?id=com.mojang.minecraftpe";

/// Most Java players own Bedrock through Microsoft, which the Android build here can't use
fn google_play_note() -> Element {
    rect()
        .horizontal()
        .spacing(8.)
        .cross_align(Alignment::Center)
        .margin(Gaps::new(6., 0., 0., 0.))
        .child(
            label()
                .text("Bedrock bought on a Microsoft account doesn't work here, macOS and Linux play the Android version.")
                .font_size(12.)
                .color(colors::fg_secondary()),
        )
        .child(
            Button::new()
                .secondary()
                .small()
                .on_press(|_| platform::open_url(GOOGLE_PLAY_URL))
                .text("Get it on Google Play"),
        )
        .into_element()
}

fn subtitle() -> &'static str {
    if cfg!(target_os = "windows") {
        "Starts Minecraft for Windows. Playing Bedrock needs a Microsoft account that owns Minecraft: Bedrock Edition."
    } else {
        "Runs the Android build through mcpelauncher. Playing Bedrock needs a Google account that owns Minecraft."
    }
}

fn game_card(
    found: &Detected,
    installing: bool,
    mut install: impl FnMut() + 'static,
    mut recheck: impl FnMut() + 'static,
) -> Element {
    let status = if installing {
        "Installing Bedrock support…".to_string()
    } else {
        bedrock::status(found)
    };

    let mut actions = rect().horizontal().spacing(8.);

    actions = match found.install.clone() {
        Some(install) => {
            let ready = cfg!(target_os = "windows") || found.version.is_some();
            let to_play = found.clone();
            let play = Button::new()
                .primary()
                .on_press(move |_| bedrock::play(&to_play));
            if ready {
                let mut row = actions.child(play.text("Play"));
                if !cfg!(target_os = "windows") {
                    row = row.child(
                        Button::new()
                            .secondary()
                            .on_press(move |_| bedrock::open_launcher(&install))
                            .text("Manage versions"),
                    );
                }
                row
            } else {
                actions
                    .child(play.text("Download the game"))
                    .child(
                        Button::new()
                            .secondary()
                            .on_press(move |_| recheck())
                            .text("Check again"),
                    )
            }
        }
        None if bedrock::can_install() => actions.child(
            Button::new()
                .primary()
                .disabled(installing)
                .on_press(move |_| install())
                .text(if installing { "Installing…" } else { "Install" }),
        ),
        None => actions
            .child(
                Button::new()
                    .primary()
                    .on_press(|_| platform::open_url(bedrock::MCPELAUNCHER_URL))
                    .text("Get mcpelauncher"),
            )
            .child(
                Button::new()
                    .secondary()
                    .on_press(move |_| recheck())
                    .text("Check again"),
            ),
    };

    if let Some(dir) = found.data_dir.clone() {
        actions = actions.child(
            Button::new()
                .secondary()
                .on_press(move |_| platform::open_path(&dir.to_string_lossy()))
                .text("Open game folder"),
        );
    }

    let warning = use_hook(bedrock::chip_warning).map(|text| {
        label()
            .text(text)
            .font_size(12.)
            .color(colors::fg_secondary())
            .into_element()
    });

    let error = (!installing)
        .then(bedrock::last_error)
        .flatten()
        .map(|err| {
            label()
                .text(format!("Install failed: {err}"))
                .font_size(12.)
                .color(colors::danger())
                .into_element()
        });

    rect()
        .vertical()
        .width(Size::fill())
        .padding(20.)
        .spacing(16.)
        .corner_radius(CornerRadius::new_all(16.))
        .background(colors::component_bg())
        .border(border_all_color(1., colors::component_border()))
        .child(
            label()
                .text(status)
                .font_size(14.)
                .color(colors::fg_primary()),
        )
        .child(actions)
        .maybe_child(warning)
        .maybe_child(error)
        .into_element()
}

/// Bedrock's Home: the instance played last up front with Play, like Java's
/// active version panel, and the other instances along the bottom
#[derive(PartialEq)]
pub struct BedrockHome;

impl Component for BedrockHome {
    fn render(&self) -> impl IntoElement {
        let found = use_hook(bedrock::detect);
        let mut list = use_state(bedrock::instances);
        let intro = use_animation(|conf| {
            conf.on_creation(OnCreation::Run);
            AnimNum::new(0., 1.).time(440).ease(Ease::Out).function(Function::Cubic)
        });
        let p = intro.get().value();

        let instances = list.read().clone();
        let last = instances.first().cloned();
        let game = found.version.as_ref().map(|v| v.file_name().unwrap_or_default().to_string_lossy().into_owned());
        let (title, subtitle) = match (&found.install, &last, &game) {
            (None, _, _) => ("Bedrock".to_string(), "mcpelauncher isn't installed yet".to_string()),
            (_, Some(inst), _) => (
                inst.name.clone(),
                format!("Bedrock {}", inst.version.clone().or(game.clone()).unwrap_or_else(|| "newest".into())),
            ),
            (_, None, Some(v)) => ("Bedrock".to_string(), format!("Bedrock {v}")),
            (_, None, None) => ("Bedrock".to_string(), "No game downloaded yet".to_string()),
        };

        let to_play = (found.clone(), last.clone());
        let play = Button::new()
            .primary()
            .medium()
            .font_size(16.)
            .font_weight(FontWeight::from(450))
            .padding(Gaps::new_symmetric(8., 24.))
            .enabled(found.install.is_some())
            .on_press(move |_| {
                match &to_play.1 {
                    Some(inst) => bedrock::play_instance(&to_play.0, inst),
                    None => bedrock::play(&to_play.0),
                }
                list.set(bedrock::instances());
            })
            .text("Play");

        let hero = rect()
            .vertical()
            .width(Size::fill())
            .main_align(Alignment::Center)
            .cross_align(Alignment::Start)
            .offset_x((p - 1.) * 48.)
            .opacity(p)
            .child(label().text(title).font_size(56.).line_height(1.1).font_weight(FontWeight::BOLD).color(colors::fg_primary()))
            .child(label().text(subtitle).font_size(16.).font_weight(FontWeight::SEMI_BOLD).color(colors::fg_secondary()))
            .child(
                rect()
                    .horizontal()
                    .spacing(8.)
                    .cross_align(Alignment::Center)
                    .margin(Gaps::new(8., 0., 0., 0.))
                    .child(play)
                    .child(
                        Button::new()
                            .ghost()
                            .icon()
                            .on_press(|_| {
                                let _ = freya::router::RouterContext::get().push(crate::Route::Bedrock {});
                            })
                            .child(crate::components::Icon::new(IconType::Settings04).size(20.)),
                    ),
            );

        // the rest of the instances, newest played first
        let mut recents = rect().horizontal().spacing(12.).opacity(p);
        for inst in instances.into_iter().skip(1).take(5) {
            let to_play = (found.clone(), inst.clone());
            let detail = inst.version.clone().unwrap_or_else(|| "Newest installed".into());
            recents = recents.child(
                rect()
                    .vertical()
                    .width(Size::px(220.))
                    .padding(16.)
                    .spacing(2.)
                    .corner_radius(CornerRadius::new_all(12.))
                    .background(colors::component_bg())
                    .border(border_all_color(1., colors::component_border()))
                    .on_press(move |_| {
                        bedrock::play_instance(&to_play.0, &to_play.1);
                        list.set(bedrock::instances());
                    })
                    .child(label().text(inst.name.clone()).font_size(15.).font_weight(FontWeight::SEMI_BOLD).color(colors::fg_primary()))
                    .child(label().text(detail).font_size(12.).color(colors::fg_secondary())),
            );
        }

        rect()
            .vertical()
            .width(Size::fill())
            .height(Size::fill())
            .content(Content::Flex)
            .overflow(Overflow::Clip)
            .padding(crate::theme::HOME_PADDING_PX)
            .child(rect().vertical().width(Size::fill()).height(Size::flex(1.0)).main_align(Alignment::Center).child(hero))
            .child(recents)
    }
}

/// Bedrock's Browse, until the CurseForge Bedrock catalog is wired in
#[derive(PartialEq)]
pub struct BedrockBrowse;

impl Component for BedrockBrowse {
    fn render(&self) -> impl IntoElement {
        rect()
            .vertical()
            .width(Size::fill())
            .height(Size::fill())
            .padding(Gaps::new(0., 40., 40., 40.))
            .spacing(6.)
            .child(label().text("Browse").font_size(36.).font_weight(FontWeight::BOLD).color(colors::fg_primary()))
            .child(
                label()
                    .text("Add-ons, texture packs, maps and shaders from CurseForge are on the way.")
                    .font_size(12.)
                    .font_weight(FontWeight::MEDIUM)
                    .color(colors::fg_secondary()),
            )
    }
}
