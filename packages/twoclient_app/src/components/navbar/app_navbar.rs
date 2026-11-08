use freya::{
    animation::*,
    prelude::*,
    router::{RouterContext, use_route},
};

use crate::{
    Route,
    components::{Avatar, Icon, IconType},
    hooks::{
        settled_or_loading, try_default_account, use_active_cluster_id, use_clusters,
        use_current_account, use_dispatch, use_notifications_snapshot,
    },
    edition::{Edition, use_edition},
    theme,
    utils::sort_clusters_for_home,
};

const NAVBAR_INTRO_MS: u64 = 460;

#[derive(PartialEq)]
pub struct Navbar;

impl Component for Navbar {
    fn render(&self) -> impl IntoElement {
        let intro = use_animation(|conf| {
            conf.on_creation(OnCreation::Run);
            AnimNum::new(0., 1.)
                .time(NAVBAR_INTRO_MS)
                .ease(Ease::Out)
                .function(Function::Cubic)
        });
        let eased = intro.get().value();
        let slide = (1.0 - eased) * -theme::NAVBAR_HEIGHT_PX;

        rect()
            .width(Size::fill())
            .height(Size::px(theme::NAVBAR_HEIGHT_PX))
            .position(Position::new_absolute().top(0.).left(0.))
            .layer(Layer::OverlayLevel(2))
            .child(
                rect()
                    .width(Size::fill())
                    .height(Size::fill())
                    .horizontal()
                    .content(Content::Flex)
                    .cross_align(Alignment::Center)
                    .padding(Gaps::new_symmetric(0.0, 40.0))
                    .offset_y(slide)
                    .opacity(eased)
                    .child(navbar_left())
                    .child(navbar_center())
                    .child(NavbarRight),
            )
            .child(
                rect()
                    .window_drag()
                    .width(Size::window_percent(100.))
                    .height(Size::px(theme::NAVBAR_HEIGHT_PX))
                    .position(Position::new_absolute().top(0.).left(0.).right(0.)),
            )
    }
}

fn navbar_left() -> impl IntoElement {
    rect()
        .horizontal()
        .width(Size::flex(1.0))
        .cross_align(Alignment::Center)
        .child(navbar_logo())
}

fn navbar_logo() -> impl IntoElement {
    // The old wordmark was an SVG of letter outlines, so it could not be
    // renamed; set in the app's own font instead
    rect()
        .horizontal()
        .cross_align(Alignment::Center)
        .spacing(10.)
        .child(
            label()
                .text("ONECLIENT")
                .font_family(theme::DEFAULT_FONT)
                .font_size(26.)
                .font_weight(FontWeight::BOLD)
                .color(theme::colors::fg_primary()),
        )
        .child(EditionSwitcher)
}

/// "for Java" next to the wordmark, opening a menu to switch editions
#[derive(PartialEq)]
struct EditionSwitcher;

impl Component for EditionSwitcher {
    fn render(&self) -> impl IntoElement {
        let edition = use_edition();
        let current = *edition.current.read();
        let mut open = use_state(|| false);
        let mut hovering = use_state(|| false);
        let menu = use_animation_with_dependencies(&open(), |conf, open| {
            conf.on_change(OnChange::Rerun);
            let (from, to) = if *open { (0., 1.) } else { (1., 0.) };
            AnimNum::new(from, to).time(160).ease(Ease::Out).function(Function::Cubic)
        });
        let shown = menu.get().value();

        let color = if hovering() || open() {
            theme::colors::fg_primary()
        } else {
            theme::colors::fg_secondary()
        };

        let mut options = rect()
            .vertical()
            .width(Size::px(180.))
            .padding(6.)
            .spacing(2.)
            .corner_radius(CornerRadius::new_all(12.))
            .background(theme::colors::component_bg())
            .border(crate::ui::border_all_color(1., theme::colors::component_border()));
        for choice in Edition::ALL {
            options = options.child(EditionOption {
                edition: choice,
                selected: choice == current,
                on_pick: EventHandler::new(move |()| {
                    open.set(false);
                    edition.switch_to(choice);
                }),
            });
        }

        rect()
            .horizontal()
            .cross_align(Alignment::Center)
            .spacing(4.)
            .padding(Gaps::new(4., 0., 0., 0.))
            .background(Color::RED.with_a(0))
            .on_press(move |e: Event<PressEventData>| {
                e.prevent_default();
                open.toggle();
            })
            .on_pointer_over(move |_| hovering.set(true))
            .on_pointer_out(move |_| hovering.set(false))
            .child(
                label()
                    .text(format!("for {}", current.label()))
                    .font_size(15.)
                    .font_weight(FontWeight::MEDIUM)
                    .color(color),
            )
            .child(Icon::new(IconType::ChevronDown).size(16.).color(color))
            .maybe_child((shown > 0.01).then(|| {
                rect()
                    .position(Position::new_absolute().top(30.).left(-6.))
                    .layer(Layer::OverlayLevel(3))
                    .opacity(shown)
                    .offset_y((1. - shown) * -6.)
                    .child(options)
                    .into_element()
            }))
    }
}

#[derive(PartialEq)]
struct EditionOption {
    edition: Edition,
    selected: bool,
    on_pick: EventHandler<()>,
}

impl Component for EditionOption {
    fn render(&self) -> impl IntoElement {
        let mut hovering = use_state(|| false);
        let on_pick = self.on_pick.clone();
        rect()
            .horizontal()
            .content(Content::Flex)
            .cross_align(Alignment::Center)
            .width(Size::fill())
            .padding(Gaps::new_symmetric(8., 10.))
            .corner_radius(CornerRadius::new_all(8.))
            .background(if hovering() { theme::colors::component_bg_hover() } else { Color::TRANSPARENT })
            .on_press(move |e: Event<PressEventData>| {
                e.stop_propagation();
                on_pick.call(());
            })
            .on_pointer_over(move |_| hovering.set(true))
            .on_pointer_out(move |_| hovering.set(false))
            .child(
                label()
                    .width(Size::flex(1.))
                    .text(self.edition.label())
                    .font_size(14.)
                    .color(theme::colors::fg_primary()),
            )
            .maybe_child(self.selected.then(|| Icon::new(IconType::Check).size(16.).color(theme::colors::brand()).into_element()))
    }
}

fn navbar_center() -> impl IntoElement {
    let route = use_route::<Route>();
    let browse_target = browse_target();
    // Bedrock reuses the same tabs; its versions and instances live on the Bedrock page
    let bedrock = *use_edition().current.read() == Edition::Bedrock;
    let (versions_active, versions_target) = if bedrock {
        (route == Route::Bedrock {}, Route::Bedrock {})
    } else {
        (route == Route::Clusters {}, Route::Clusters {})
    };
    let (browse_active, browse_target) = if bedrock {
        (route == Route::BedrockBrowse {}, Route::BedrockBrowse {})
    } else {
        (matches!(route, Route::Browser { pick_cluster: true, .. }), browse_target)
    };

    rect()
        .horizontal()
        .width(Size::flex(1.0))
        .main_align(Alignment::Center)
        .cross_align(Alignment::Center)
        .spacing(36.)
        .child(NavLink {
            active: route == Route::Home {},
            target: NavTarget::Route(Route::Home {}),
            nav_label: "Home",
        })
        .child(NavLink {
            active: versions_active,
            target: NavTarget::Route(versions_target),
            nav_label: "Versions",
        })
        .child(NavLink {
            active: browse_active,
            target: NavTarget::Route(browse_target),
            nav_label: "Browse",
        })
        .child(NavLink {
            active: route == Route::Stats {},
            target: NavTarget::Route(Route::Stats {}),
            nav_label: "Stats",
        })
}

/// Falls back active cluster then most recently played then Versions when none exist
fn browse_target() -> Route {
    let clusters = settled_or_loading(&use_clusters()).unwrap_or_default();
    let active = *use_active_cluster_id().read();

    let cluster_id = active
        .filter(|id| clusters.iter().any(|cluster| cluster.id == *id))
        .or_else(|| sort_clusters_for_home(clusters).first().map(|c| c.id));

    match cluster_id {
        Some(cluster_id) => Route::Browser {
            cluster_id,
            package_type: "mod".to_string(),
            pick_cluster: true,
        },
        None => Route::Clusters {},
    }
}

#[derive(PartialEq, Clone)]
enum NavTarget {
    Route(Route),
}

#[derive(PartialEq)]
struct NavLink {
    active: bool,
    target: NavTarget,
    nav_label: &'static str,
}

impl Component for NavLink {
    fn render(&self) -> impl IntoElement {
        let mut hovering = use_state(|| false);
        let a11y_id = use_a11y();
        let focused = use_focus(a11y_id);

        let active = self.active;
        let target = self.target.clone();
        let nav_label = self.nav_label;

        let color = if active || hovering() || focused().is_focused() {
            theme::colors::fg_primary()
        } else {
            theme::colors::fg_secondary()
        };

        let underline_width = if active {
            27.
        } else if hovering() || focused().is_focused() {
            18.
        } else {
            0.
        };

        rect()
            .vertical()
            .cross_align(Alignment::Center)
            .spacing(2.)
            .width(Size::px(nav_label.len() as f32 * 10. + 10.))
            // TODO workaround for a Freya measurement bug a fully transparent background
            // measures wrongly so give it alpha 0 red to keep pointer events working
            .background(Color::RED.with_a(0))
            .a11y_id(a11y_id)
            .a11y_focusable(true)
            .a11y_role(AccessibilityRole::Button)
            .on_press(move |e: Event<PressEventData>| {
                e.prevent_default();
                match &target {
                    NavTarget::Route(route) => {
                        let _ = RouterContext::get().push(route.clone());
                    }
                }
            })
            .on_pointer_over(move |_| hovering.set(true))
            .on_pointer_out(move |_| hovering.set(false))
            .cursor(CursorIcon::Pointer)
            .child(
                label()
                    .text(nav_label)
                    .font_size(16.)
                    .font_weight(if active {
                        FontWeight::MEDIUM
                    } else {
                        FontWeight::NORMAL
                    })
                    .color(color),
            )
            .child(
                rect()
                    .height(Size::px(2.))
                    .width(Size::px(underline_width))
                    .corner_radius(CornerRadius::new_all(2.))
                    .background(if active {
                        theme::colors::fg_primary()
                    } else {
                        theme::colors::fg_secondary()
                    }),
            )
    }
}

#[derive(PartialEq)]
struct NavbarRight;

impl Component for NavbarRight {
    fn render(&self) -> impl IntoElement {
        let current_account = use_current_account();
        let dispatch = use_dispatch();
        let unread = use_notifications_snapshot().unread_count();

        let account_uuid = try_default_account(&current_account)
            .map(|account| account.id.to_string())
            .unwrap_or_else(|| uuid::Uuid::nil().to_string());

        let notif_dispatch = dispatch.clone();
        let open_notifications = move |_| {
            notif_dispatch.toggle_notification_center();
        };

        let open_account_switcher = move |_| {
            dispatch.toggle_account_switcher();
        };

        let open_settings = |_| {
            let _ = RouterContext::get().push(Route::SettingsLauncher {});
        };

        rect()
            .horizontal()
            .width(Size::flex(1.0))
            .main_align(Alignment::End)
            .cross_align(Alignment::Center)
            .spacing(8.)
            .child(
                super::navbar_button()
                    .child(notification_bell(unread))
                    .on_press(open_notifications),
            )
            .child(
                super::navbar_button()
                    .child(Icon::new(IconType::Settings02).size(20.))
                    .on_press(open_settings),
            )
            .child(
                super::navbar_button()
                    .padding(0.0)
                    .on_press(open_account_switcher)
                    .child(
                        Avatar::new(account_uuid)
                            .width(Size::px(24.))
                            .height(Size::px(24.)),
                    ),
            )
            .child(super::window_controls())
    }
}

fn notification_bell(unread: usize) -> impl IntoElement {
    rect()
        .width(Size::px(20.))
        .height(Size::px(20.))
        .child(Icon::new(IconType::Bell01).size(20.))
        .maybe_child((unread > 0).then(|| {
            rect()
                .position(Position::new_absolute().top(-4.).right(-4.))
                .width(Size::px(16.))
                .height(Size::px(16.))
                .corner_radius(CornerRadius::from(8.))
                .background(theme::colors::danger())
                .layer(Layer::Relative(3))
                .center()
                .child(
                    label()
                        .text(if unread > 9 {
                            "9+".to_string()
                        } else {
                            unread.to_string()
                        })
                        .font_size(10.)
                        .font_weight(FontWeight::SEMI_BOLD)
                        .color(theme::colors::fg_primary()),
                )
                .into_element()
        }))
}
