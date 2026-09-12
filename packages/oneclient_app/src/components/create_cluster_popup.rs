//! Asks for the three things an instance cannot be created without: a name, a
//! Minecraft version, and a loader
//!
//! The version list is Mojang's, not the curated one the version cards are
//! built from, so anything the game has ever shipped can be made into an
//! instance. Everything else about the instance is editable afterwards, so the
//! dialog stays at three fields.

use freya::prelude::*;
use oneclient_common::domain::GameLoader;

use crate::components::{Button, Dropdown, OverlayPopup, TextInput, toggle_controlled};
use crate::hooks::{
    use_active_cluster_id, use_dispatch, use_game_versions, use_loaders_for_version,
    game_versions, loaders_for_version,
};
use crate::theme::colors;
use crate::ui::border_all_color;

const CARD_BG: Color = Color::from_rgb(26, 34, 41);
const CARD_WIDTH_PX: f32 = 460.;

/// Long enough to be worth scrolling, short enough that the list stays usable
const VERSION_LIMIT: usize = 300;

#[derive(PartialEq)]
pub struct CreateClusterPopup {
    pub on_close: EventHandler<()>,
}

impl Component for CreateClusterPopup {
    fn render(&self) -> impl IntoElement {
        let dispatch = use_dispatch();
        let active_id = use_active_cluster_id();

        let name = use_state(String::new);
        let mut snapshots = use_state(|| false);
        let mut dedicated = use_state(|| true);
        let mut version_index = use_state(|| 0_usize);
        let mut loader_index = use_state(|| 0_usize);

        let all_versions = game_versions(&use_game_versions());
        let versions: Vec<String> = all_versions
            .iter()
            .filter(|version| {
                *snapshots.read()
                    || matches!(version.type_, oneclient_core::VersionType::Release)
            })
            .map(|version| version.id.clone())
            .take(VERSION_LIMIT)
            .collect();

        // A late-settling list, or one that just lost its snapshots, can leave
        // the index past the end
        let picked_version = versions
            .get(*version_index.read())
            .or_else(|| versions.first())
            .cloned();

        let loaders = match &picked_version {
            Some(version) => loaders_for_version(&use_loaders_for_version(version.clone())),
            None => vec![GameLoader::Vanilla],
        };
        let loader_labels: Vec<String> =
            loaders.iter().map(|loader| loader.to_string()).collect();
        let picked_loader = loaders
            .get(*loader_index.read())
            .copied()
            .unwrap_or(GameLoader::Vanilla);

        let trimmed = name.read().trim().to_string();
        let can_create = !trimmed.is_empty() && picked_version.is_some();

        let close_on_cancel = self.on_close.clone();
        let close_on_create = self.on_close.clone();
        let close_on_scrim = self.on_close.clone();
        let create_version = picked_version.clone();

        OverlayPopup::new()
            .on_close(move |_| close_on_scrim.call(()))
            .child(
                rect()
                    .width(Size::window_percent(100.))
                    .height(Size::window_percent(100.))
                    .center()
                    .child(
                        rect()
                            .vertical()
                            .width(Size::px(CARD_WIDTH_PX))
                            .max_width(Size::window_percent(90.))
                            .spacing(14.)
                            .padding(Gaps::new_all(20.))
                            .corner_radius(CornerRadius::new_all(14.))
                            .background(CARD_BG)
                            .border(border_all_color(1., colors::component_border()))
                            .child(
                                label()
                                    .text("New instance")
                                    .font_size(16.)
                                    .font_weight(FontWeight::SEMI_BOLD)
                                    .color(colors::fg_primary()),
                            )
                            .child(field(
                                "Name",
                                TextInput::new(name)
                                    .placeholder("My instance")
                                    .auto_focus(true)
                                    .width(Size::fill())
                                    .into_element(),
                            ))
                            .child(field(
                                "Minecraft version",
                                match picked_version.clone() {
                                    Some(current) => Dropdown::new(current, versions.clone())
                                        .width(Size::fill())
                                        .on_select(move |idx: usize| {
                                            *version_index.write() = idx;
                                            // A different version is a different
                                            // set of loaders
                                            *loader_index.write() = 0;
                                        })
                                        .into_element(),
                                    None => hint("Loading versions..."),
                                },
                            ))
                            .child(switch_row(
                                *snapshots.read(),
                                EventHandler::new(move |()| {
                                    let showing = *snapshots.peek();
                                    snapshots.set(!showing);
                                    // The list under the index just changed
                                    *version_index.write() = 0;
                                    *loader_index.write() = 0;
                                }),
                                "Show snapshots",
                                "",
                            ))
                            .child(field(
                                "Mod loader",
                                Dropdown::new(
                                    loader_labels
                                        .get(*loader_index.read())
                                        .cloned()
                                        .unwrap_or_else(|| GameLoader::Vanilla.to_string()),
                                    loader_labels.clone(),
                                )
                                .width(Size::fill())
                                .on_select(move |idx: usize| *loader_index.write() = idx)
                                .into_element(),
                            ))
                            .child(switch_row(
                                *dedicated.read(),
                                EventHandler::new(move |()| {
                                    let on = *dedicated.peek();
                                    dedicated.set(!on);
                                }),
                                "Separate game folder",
                                "Its own saves, options and screenshots, instead of the \
                                 folder every other instance shares.",
                            ))
                            .child(
                                rect()
                                    .horizontal()
                                    .width(Size::fill())
                                    .main_align(Alignment::End)
                                    .spacing(8.)
                                    .child(
                                        Button::new()
                                            .secondary()
                                            .text("Cancel")
                                            .on_press(move |_| close_on_cancel.call(())),
                                    )
                                    .child(
                                        Button::new()
                                            .primary()
                                            .text("Create")
                                            .enabled(can_create)
                                            .on_press(move |_| {
                                                let Some(version) = create_version.clone() else {
                                                    return;
                                                };
                                                dispatch.create_cluster(
                                                    oneclient_core::CreateClusterOptions::new(
                                                        trimmed.clone(),
                                                        version,
                                                        picked_loader,
                                                    ),
                                                    *dedicated.peek(),
                                                    active_id,
                                                );
                                                close_on_create.call(());
                                            }),
                                    ),
                            ),
                    ),
            )
    }
}

fn switch_row(on: bool, on_toggle: EventHandler<()>, title: &str, note: &str) -> Element {
    let text = rect()
        .vertical()
        .width(Size::flex(1.0))
        .spacing(2.)
        .child(
            label()
                .text(title.to_string())
                .font_size(12.)
                .color(colors::fg_primary()),
        )
        .maybe_child((!note.is_empty()).then(|| {
            label()
                .text(note.to_string())
                .font_size(11.)
                .color(colors::fg_secondary())
                .into_element()
        }));

    rect()
        .horizontal()
        .width(Size::fill())
        .cross_align(Alignment::Center)
        .spacing(10.)
        .child(text)
        .child(toggle_controlled(on, on_toggle))
        .into_element()
}

fn field(title: &str, control: Element) -> Element {
    rect()
        .vertical()
        .width(Size::fill())
        .spacing(6.)
        .child(
            label()
                .text(title.to_string())
                .font_size(12.)
                .font_weight(FontWeight::MEDIUM)
                .color(colors::fg_secondary()),
        )
        .child(control)
        .into_element()
}

fn hint(text: &str) -> Element {
    label()
        .text(text.to_string())
        .font_size(12.)
        .color(colors::fg_secondary())
        .into_element()
}
