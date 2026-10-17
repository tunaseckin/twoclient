//! Core operations go onto freya's UI-thread executor radio state is `!Send`
//!
//! Always `spawn_forever` never `spawn` Freya's `spawn` cancels the task when
//! the calling component unmounts this work is app-scoped not component-scoped

use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, Instant};

use freya::prelude::{State, spawn_forever};
use freya::radio::RadioStation;
use twoclient_cluster::{
    ClusterStage, ClusterUpdate, GameSettingsProfile, PackageUpdateMode, ProfileUpdate,
};
use twoclient_common::domain::{ContentType, ProviderId};
use twoclient_content::packages::{LiveSync, LocalImportReport};
use twoclient_core::relocate::RelocationPlan;
use twoclient_core::settings::LauncherSettings;
use twoclient_core::settings::store::{save_global_profile, save_settings_and_apply};
use twoclient_db::models::ClusterId;
use twoclient_events::{Answer, Level};
use tokio::sync::mpsc;

use crate::components::IconType;
use crate::{invalidate_java_queries, launcher};
use crate::notifications::{
    ClusterUpdateSummary, NotificationAction, NotificationSpec, OptionalModRef, OptionalModsGroup,
    PackageUpdateGroup, PendingPrompt,
};
use crate::state::{AppChannel, AppState, AsyncStatus, RelocationState};

/// Over-disabling is the cheaper mistake a dead button beats a second game
const LAUNCH_HOLD: Duration = Duration::from_secs(2);

/// Kept pure and separate from the work it describes so the three-way mode is
/// testable without a launcher database or provider
///
/// `pending` has already had declined versions filtered out by
/// `BrowserUpdateCheck::pending`

#[derive(Debug, Clone, PartialEq)]
enum PackageUpdatePlan {
    Nothing,
    Apply(Vec<twoclient_core::BrowserPackageUpdate>),
    Prompt(Vec<twoclient_core::BrowserPackageUpdate>),
}

/// Kept pure and separate from the work it describes so the three-way mode is
/// testable without a launcher database or provider
///
/// `pending` has already had declined versions filtered out by
/// `BrowserUpdateCheck::pending`
fn plan_package_updates(
    mode: PackageUpdateMode,
    pending: Vec<twoclient_core::BrowserPackageUpdate>,
) -> PackageUpdatePlan {
    if pending.is_empty() {
        return PackageUpdatePlan::Nothing;
    }

    match mode {
        // The cache was already written skipping only affects what is shown
        PackageUpdateMode::Skip => PackageUpdatePlan::Nothing,
        PackageUpdateMode::Automatic => PackageUpdatePlan::Apply(pending),
        PackageUpdateMode::Prompt => PackageUpdatePlan::Prompt(pending),
    }
}

/// The rows a bulk enable/disable would actually touch
///
/// A row with neither a hash nor a bundle has nothing to flip: it is a browser
/// result, not something installed. One already in the target state is skipped
/// so the count the user is shown is the count that changed.
pub fn packages_to_flip(
    rows: &[crate::components::PackageEntry],
    enabled: bool,
) -> Vec<crate::components::PackageEntry> {
    rows.iter()
        .filter(|p| p.hash.is_some() || p.bundle_name.is_some())
        .filter(|p| p.enabled != enabled)
        .cloned()
        .collect()
}

/// The pump alone owns the toast timers so adding or removing a toast has to
/// tell it to re-arm hover-pause is a timer property not component state
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PumpSignal {
    Reconcile,
    PauseToasts,
    ResumeToasts,
}

#[derive(Clone)]
pub struct Actions {
    station: RadioStation<AppState, AppChannel>,
    pump: mpsc::UnboundedSender<PumpSignal>,
    events: twoclient_events::EventBus,
}

impl Actions {
    #[must_use]
    pub fn station(&self) -> RadioStation<AppState, AppChannel> {
        self.station
    }

    /// A channel handle, so a clone still feeds the pump set up at launch
    #[must_use]
    pub fn events(&self) -> twoclient_events::EventBus {
        self.events.clone()
    }

    #[must_use]
    pub fn new(
        station: RadioStation<AppState, AppChannel>,
        pump: mpsc::UnboundedSender<PumpSignal>,
        events: twoclient_events::EventBus,
    ) -> Self {
        Self {
            station,
            pump,
            events,
        }
    }

    fn nudge(&self, signal: PumpSignal) {
        let _ = self.pump.send(signal);
    }


    fn with_engine(&self, mutate: impl FnOnce(&mut AppState)) {
        {
            let mut guard = self.station.clone().write_channel(AppChannel::Notifications);
            mutate(&mut guard);
        }
        self.nudge(PumpSignal::Reconcile);
    }

    fn set_settings_error(&self, error: Option<String>) {
        self.station
            .clone()
            .write_channel(AppChannel::Settings)
            .settings
            .error = error;
    }

    pub fn reload_settings(&self) {
        let station = self.station;
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            {
                let mut guard = station.clone().write_channel(AppChannel::Settings);
                guard.settings.status = AsyncStatus::Loading;
                guard.settings.error = None;
            }

            let loaded = twoclient_core::settings::store::load_settings(Some(
                &state.services.events,
            ))
            .await;
            let discord_enabled = loaded.discord_enabled;
            *state.settings.write() = loaded.clone();
            state.discord.set_enabled(discord_enabled);

            let mut guard = station.clone().write_channel(AppChannel::Settings);
            guard.settings.settings = loaded;
            guard.settings.status = AsyncStatus::Ready;
            guard.settings.error = None;
        });
    }

    pub fn save_settings(&self) {
        let actions = self.clone();
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            {
                let mut guard = actions.station.clone().write_channel(AppChannel::Settings);
                guard.settings.saving = true;
                guard.settings.error = None;
            }

            let settings = state.settings.read().clone();
            let result = save_settings_and_apply(&state.services, &settings).await;

            let mut guard = actions.station.clone().write_channel(AppChannel::Settings);
            guard.settings.error = result.err().map(|err| err.to_string());
            guard.settings.saving = false;
            guard.settings.status = AsyncStatus::Ready;
        });
    }

    fn mutate_settings(&self, mutate: impl FnOnce(&mut LauncherSettings)) -> Option<LauncherSettings> {
        let state = launcher::state().ok()?;
        let updated = {
            let mut lock = state.settings.write();
            mutate(&mut lock);
            lock.clone()
        };

        let mut guard = self.station.clone().write_channel(AppChannel::Settings);
        guard.settings.settings = updated.clone();
        guard.settings.status = AsyncStatus::Ready;
        Some(updated)
    }

    fn persist(&self, settings: LauncherSettings) {
        let actions = self.clone();
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            if let Err(err) = save_settings_and_apply(&state.services, &settings).await {
                actions.set_settings_error(Some(err.to_string()));
            }
        });
    }

    pub fn set_settings(&self, settings: LauncherSettings) {
        let Some(updated) = self.mutate_settings(|s| *s = settings) else {
            return;
        };
        if let Ok(state) = launcher::state() {
            state.discord.set_enabled(updated.discord_enabled);
        }
        self.persist(updated);
    }

    /// Not persisted the next real save carries the seen versions along
    pub fn record_seen_version(&self, version: impl Into<String>) {
        let version = version.into();
        self.mutate_settings(|settings| {
            if !settings.seen_versions.iter().any(|v| v == &version) {
                settings.seen_versions.push(version);
            }
        });
    }

    pub fn set_seen_changelog_version(&self, version: impl Into<String>) {
        let version = version.into();
        let mut changed = false;
        let updated = self.mutate_settings(|settings| {
            if settings.seen_changelog_version.as_deref() != Some(version.as_str()) {
                settings.seen_changelog_version = Some(version);
                changed = true;
            }
        });

        if let (true, Some(updated)) = (changed, updated) {
            self.persist(updated);
        }
    }

    pub fn mark_onboarding_seen(&self) {
        if let Some(updated) = self.mutate_settings(|settings| settings.seen_onboarding = true) {
            self.persist(updated);
        }
    }

    pub fn reset_onboarding(&self) {
        if let Some(updated) = self.mutate_settings(|settings| {
            settings.seen_onboarding = false;
            settings.accepted_tos_version = 0;
            settings.accepted_privacy_version = 0;
            settings.seen_versions.clear();
        }) {
            self.persist(updated);
        }
    }

    pub fn accept_tos(&self, terms_version: u32, privacy_version: u32) {
        let mut was_declined = false;

        let Some(updated) = self.mutate_settings(|settings| {
            was_declined = settings.declined_tos;
            settings.accepted_tos_version = terms_version;
            settings.accepted_privacy_version = privacy_version;
            settings.declined_tos = false;
        }) else {
            return;
        };

        self.persist(updated);

        if was_declined {
            self.notify("Restart to finish")
                .body(
                    "Thanks. TwoClient reconnects to Polyfrost services the next time you start \
                     it.",
                )
                .icon(IconType::RefreshCw01)
                .send();
        }
    }

    pub fn decline_tos(&self) {
        twoclient_common::consent::decline();

        if let Some(updated) = self.mutate_settings(|settings| {
            settings.declined_tos = true;
            settings.accepted_tos_version = 0;
            settings.accepted_privacy_version = 0;
        }) {
            self.persist(updated);
        }
    }

    pub fn save_global_profile(&self, profile: GameSettingsProfile) {
        let actions = self.clone();
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            if let Err(err) = save_global_profile(&state.settings, profile).await {
                tracing::error!("failed to save the global profile: {err:#}");
                return;
            }
            actions.mutate_settings(|_| {});
            super::invalidate_profile_queries().await;
        });
    }

    pub fn update_global_profile(&self, update: ProfileUpdate) {
        let actions = self.clone();
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            let mut global = state.settings.read().global_game_settings.clone();
            update.apply(&mut global);
            if let Err(err) = save_global_profile(&state.settings, global).await {
                tracing::error!("failed to update the global profile: {err:#}");
                return;
            }
            actions.mutate_settings(|_| {});
            super::invalidate_profile_queries().await;
        });
    }

    pub fn create_settings_profile(&self, name: impl Into<String>) {
        let name = name.into();
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            let global = state.settings.read().global_game_settings.clone();
            match twoclient_cluster::profiles::create_settings_profile(
                &state.services.db,
                &global,
                &name,
            )
            .await
            {
                Ok(_) => super::invalidate_profile_queries().await,
                Err(err) => tracing::error!("failed to create settings profile: {err:#}"),
            }
        });
    }

    pub fn create_profile_from_global(
        &self,
        name: impl Into<String>,
        mem_max: Option<u32>,
        force_fullscreen: Option<bool>,
    ) {
        let name = name.into();
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            let global = state.settings.read().global_game_settings.clone();
            match twoclient_cluster::profiles::create_profile_from_global(
                &state.services.db,
                &global,
                &name,
                mem_max,
                force_fullscreen,
            )
            .await
            {
                Ok(_) => super::invalidate_profile_queries().await,
                Err(err) => tracing::error!("failed to create profile: {err:#}"),
            }
        });
    }

    pub fn upsert_named_profile(&self, profile: GameSettingsProfile) {
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            match twoclient_cluster::profiles::upsert_named_profile(&state.services.db, &profile)
                .await
            {
                Ok(_) => super::invalidate_profile_queries().await,
                Err(err) => tracing::error!("failed to save profile: {err:#}"),
            }
        });
    }

    pub fn update_named_profile(&self, name: impl Into<String>, update: ProfileUpdate) {
        let name = name.into();
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            match twoclient_cluster::profiles::update_named_profile(
                &state.services.db,
                &name,
                update,
            )
            .await
            {
                Ok(_) => super::invalidate_profile_queries().await,
                Err(err) => tracing::error!("failed to update profile: {err:#}"),
            }
        });
    }

    pub fn delete_named_profile(&self, name: impl Into<String>) {
        let name = name.into();
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            match twoclient_cluster::profiles::delete_named_profile(&state.services.db, &name).await
            {
                Ok(()) => super::invalidate_profile_queries().await,
                Err(err) => tracing::error!("failed to delete profile: {err:#}"),
            }
        });
    }

    pub fn update_cluster_profile(&self, cluster_id: ClusterId, update: ProfileUpdate) {
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            match state.clusters.update_profile(cluster_id, update).await {
                Ok(_) => super::invalidate_profile_queries().await,
                Err(err) => tracing::error!("failed to update cluster profile: {err:#}"),
            }
        });
    }

    pub fn create_and_assign_cluster_profile(
        &self,
        cluster_id: ClusterId,
        profile_name: impl Into<String>,
    ) {
        let profile_name = profile_name.into();
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            let global = state.settings.read().global_game_settings.clone();
            match state
                .clusters
                .create_and_assign_profile(&global, cluster_id, &profile_name)
                .await
            {
                Ok(_) => super::invalidate_profile_queries().await,
                Err(err) => tracing::error!("failed to assign cluster profile: {err:#}"),
            }
        });
    }

    pub fn set_cluster_loader_version(&self, cluster_id: ClusterId, version: impl Into<String>) {
        let version = version.into();
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            if let Err(err) = state
                .clusters
                .update(cluster_id, ClusterUpdate::default().loader_version(version))
                .await
            {
                tracing::error!("failed to set loader version: {err:#}");
                return;
            }
            // Best effort on failure the cluster just will not re-download
            let _ = state
                .clusters
                .set_stage(cluster_id, ClusterStage::NotReady)
                .await;
            super::invalidate_cluster_queries().await;
        });
    }

    pub fn import_launcher(
        &self,
        source: twoclient_core::MigrationSource,
        folder_name: impl Into<String>,
        target: twoclient_core::ImportTarget,
    ) {
        let folder_name = folder_name.into();
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            let events = state.services.events.clone();
            match twoclient_core::import_migration_game_dir(&state, source, &folder_name, target)
                .await
            {
                Ok(()) => {
                    events
                        .notify("Import complete")
                        .body(format!("Copied your files from {}.", source.display_name()))
                        .send();
                    events.signal(twoclient_events::Signal::ClustersChanged);
                }
                Err(err) => events
                    .notify("Import failed")
                    .body(err.to_string())
                    .error()
                    .send(),
            }
        });
    }

    pub fn install_java_runtime(&self, vendor: twoclient_java::JavaVendor, major: u32) {
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            let events = state.services.events.clone();
            match state.java.install_runtime_from(&vendor, major).await {
                Ok(_) => {
                    events.signal(twoclient_events::Signal::JavaChanged);
                    invalidate_java_queries().await
                },
                Err(err) => {
					events
                    .notify("Java install failed")
                    .body(err.to_string())
                    .error()
                    .send()
				}
            }
        });
    }

    pub fn add_custom_java_runtime(&self, path: PathBuf) {
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            let events = state.services.events.clone();
            match state.java.add_custom_runtime(path).await {
                Ok(runtime) => {
                    events
                        .notify("Java added")
                        .body(format!("Java {} ({})", runtime.major, runtime.vendor))
                        .send();
                    events.signal(twoclient_events::Signal::JavaChanged);
                }
                Err(err) => events
                    .notify("Failed to add Java")
                    .body(err.to_string())
                    .error()
                    .send(),
            }
        });
    }

    pub fn remove_java_runtime(&self, path: impl Into<String>) {
        let path = path.into();
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            match state.java.remove_runtime(&path).await {
                Ok(()) => super::invalidate_java_queries().await,
                Err(err) => tracing::error!("failed to remove java runtime: {err:#}"),
            }
        });
    }

    pub fn toggle_notification_center(&self) {
        self.with_engine(|state| {
            state.center_open = state
                .notifications
                .toggle_center(&mut state.inbox, state.center_open);
        });
    }

    pub fn toggle_account_switcher(&self) {
        let open = self.station.peek().account_switcher_open;
        self.station
            .clone()
            .write_channel(AppChannel::AccountSwitcher)
            .account_switcher_open = !open;
    }

    pub fn close_account_switcher(&self) {
        self.station
            .clone()
            .write_channel(AppChannel::AccountSwitcher)
            .account_switcher_open = false;
    }

    pub fn close_notification_center(&self) {
        self.with_engine(|state| state.center_open = false);
    }

    pub fn clear_notification_inbox(&self) {
        self.with_engine(|state| {
            state.inbox.clear();
            state.notifications.clear_inbox();
        });
    }

    pub fn dismiss_toast(&self, entry_id: u64) {
        self.with_engine(|state| state.notifications.dismiss_toast(&mut state.inbox, entry_id));
    }

    pub fn mark_notification_read(&self, entry_id: u64) {
        self.with_engine(|state| state.notifications.mark_read(&mut state.inbox, entry_id));
    }

    pub fn dismiss_notification(&self, entry_id: u64) {
        self.with_engine(|state| {
            state
                .notifications
                .dismiss_notification(&mut state.inbox, entry_id);
        });
    }

    pub fn open_cluster_update(&self, summaries: Vec<ClusterUpdateSummary>) {
        self.with_engine(|state| {
            state.notifications.open_cluster_update(summaries);
            state.center_open = false;
        });
    }

    pub fn close_cluster_update(&self) {
        self.with_engine(|state| state.notifications.close_cluster_update());
    }

    pub fn open_optional_mods(
        &self,
        groups: Vec<OptionalModsGroup>,
        done: Option<tokio::sync::oneshot::Sender<()>>,
    ) {
        self.with_engine(move |state| state.notifications.open_optional_mods(groups, done));
    }

    pub fn close_optional_mods(&self) {
        self.with_engine(|state| state.notifications.finish_optional_mods());
    }

    pub fn decline_optional_mods(&self, mods: Vec<(ClusterId, OptionalModRef)>) {
        self.close_optional_mods();
        self.record_skipped_optional_mods(mods);
    }

    pub fn record_skipped_optional_mods(&self, mods: Vec<(ClusterId, OptionalModRef)>) {
        if mods.is_empty() {
            return;
        }

        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            let content = state.services.content();

            for (cluster_id, package_ids) in group_by_cluster(&mods) {
                if let Err(err) =
                    twoclient_core::skip_optional_mods(cluster_id, &package_ids, &content).await
                {
                    tracing::warn!(
                        cluster_id,
                        error = %err,
                        "failed to record skipped optional mods, they will come back as new"
                    );
                }
            }
        });
    }

    /// Holds the launch until the mods are actually on disk Minecraft reads
    /// `mods/` once at startup, so finishing after the process starts is useless
    pub fn enable_optional_mods(&self, mods: Vec<(ClusterId, OptionalModRef)>) {
        if mods.is_empty() {
            self.close_optional_mods();
            return;
        }

        // Off screen straight away, but the launch keeps waiting until the
        // install below is done, so the pause needs a progress notification of
        // its own the same way `apply_updates_for_launch` gives one
        self.with_engine(|state| state.notifications.hide_optional_mods());
        let actions = self.clone();

        spawn_forever(async move {
            let Ok(state) = launcher::state() else {
                actions.close_optional_mods();
                return;
            };
            let content = state.services.content();
            let events = state.services.events.clone();
            let session = twoclient_events::GroupedProgressSession::start(
                &events,
                "Adding optional mods",
            );
            let opt_in = session.child(
                "Enabling mods",
                mods.len() as u64,
                twoclient_events::TaskCategory::Packages,
            );
            opt_in.set_phase(twoclient_events::TaskPhase::Installing);

            let mut clusters: Vec<ClusterId> = Vec::new();
            let mut enabled: Vec<(ClusterId, OptionalModRef)> = Vec::new();
            let mut failed = 0usize;
            for (cluster_id, (bundle_name, package_id)) in &mods {
                match twoclient_core::set_bundle_package_enabled(
                    *cluster_id,
                    bundle_name,
                    package_id,
                    true,
                    false,
                    &content,
                )
                .await
                {
                    Ok(()) => {
                        if !clusters.contains(cluster_id) {
                            clusters.push(*cluster_id);
                        }
                        enabled.push((*cluster_id, (bundle_name.clone(), package_id.clone())));
                    }
                    Err(err) => {
                        failed += 1;
                        tracing::warn!(
                            cluster_id,
                            %bundle_name,
                            %package_id,
                            error = %err,
                            "failed to opt in to an optional mod"
                        );
                    }
                }
            }

            opt_in.finish();

            let mut installed = 0usize;
            let mut applied: Vec<ClusterId> = Vec::new();
            for cluster_id in &clusters {
                match twoclient_core::apply_bundle_updates_with(
                    *cluster_id,
                    state.bundles.as_ref(),
                    &content,
                    Some(&session),
                )
                .await
                {
                    Ok(result) => {
                        installed += result.additions_applied.len();
                        failed += result.additions_failed.len();
                        applied.push(*cluster_id);
                    }
                    Err(err) => {
                        tracing::warn!(cluster_id, error = %err, "failed to install optional mods");
                    }
                }
            }

            // An offer only counts as answered once its mod really was enabled
            // and installed otherwise the click is lost and never comes back
            enabled.retain(|(cluster_id, _)| applied.contains(cluster_id));
            for (cluster_id, package_ids) in group_by_cluster(&enabled) {
                if let Err(err) =
                    twoclient_core::resolve_optional_mods(cluster_id, &package_ids, &content).await
                {
                    tracing::warn!(cluster_id, error = %err, "failed to clear answered offers");
                }
            }

            session.finish();
            super::invalidate_cluster_queries().await;

            if installed > 0 {
                events
                    .notify("Mods added")
                    .body(format!(
                        "{installed} mod{} added",
                        if installed == 1 { "" } else { "s" }
                    ))
                    .send();
            }
            if failed > 0 {
                events
                    .notify("Some mods were not added")
                    .body(format!(
                        "{failed} mod{} could not be installed",
                        if failed == 1 { "" } else { "s" }
                    ))
                    .error()
                    .send();
            }

            actions.close_optional_mods();
        });
    }

    pub fn close_package_updates(&self) {
        self.with_engine(|state| state.notifications.close_package_updates());
    }

    /// Hovering any toast pauses every toast including ones arriving while
    /// hovering hence a pump signal rather than a state flag
    pub fn pause_toasts(&self) {
        self.nudge(PumpSignal::PauseToasts);
    }

    pub fn resume_toasts(&self) {
        self.nudge(PumpSignal::ResumeToasts);
    }

    pub fn answer_prompt(&self, answer: Answer) {
        self.reply_to_prompt(Some(answer));
    }

    pub fn dismiss_prompt(&self) {
        self.reply_to_prompt(None);
    }

    fn reply_to_prompt(&self, answer: Option<Answer>) {
        let taken = self
            .station
            .clone()
            .write_channel(AppChannel::Notifications)
            .prompt
            .take();

        if let Some(PendingPrompt {
            reply_tx: Some(reply_tx),
            ..
        }) = taken
        {
            let _ = reply_tx.send(answer);
        }
    }

    pub fn notify(&self, title: impl Into<String>) -> NotificationBuilder {
        NotificationBuilder {
            actions: self.clone(),
            spec: NotificationSpec {
                title: title.into(),
                body: String::new(),
                level: Level::Info,
                icon: None,
                progress: None,
                actions: Vec::new(),
            },
        }
    }

    fn push_notification(&self, spec: NotificationSpec) {
        self.with_engine(|state| {
            state.notifications.push_custom(&mut state.inbox, spec);
        });
    }

    pub fn send_test_progress(&self, current: u64, total: u64) {
        let Ok(state) = launcher::state() else { return };
        let id = uuid::Uuid::from_u128(0x0CE0_0CE0_0CE0_0CE0_0CE0_0CE0_0CE0_0CE0);
        state
            .services
            .events
            .progress(id, "Downloading assets", current, total);
    }

    pub fn launch_cluster(&self, cluster_id: ClusterId) {
        let claimed = self
            .station
            .clone()
            .write_channel(AppChannel::Game)
            .game
            .begin_launch(cluster_id);
        if !claimed {
            return;
        }

        let station = self.station;
        let actions = self.clone();
        spawn_forever(async move {
            let started = Instant::now();
            launch(&actions, cluster_id).await;

            // An instant failure would hand the button back inside the same
            // click burst so hold it for the floor either way
            if let Some(remaining) = LAUNCH_HOLD.checked_sub(started.elapsed()) {
                tokio::time::sleep(remaining).await;
            }
            station
                .clone()
                .write_channel(AppChannel::Game)
                .game
                .finish_launch(cluster_id);
        });
    }

    pub fn kill_cluster(&self, cluster_id: ClusterId) {
        let Ok(state) = launcher::state() else { return };
        if !state.games.kill(cluster_id) {
            tracing::debug!(cluster_id, "kill requested but no tracked process");
        }
    }

    pub fn dismiss_game_error(&self) {
        self.station
            .clone()
            .write_channel(AppChannel::Game)
            .game
            .error = None;
    }

    pub fn import_local_file(&self, cluster_id: ClusterId, content_type: ContentType, path: PathBuf) {
        self.import_local_files(cluster_id, vec![(path, content_type)]);
    }

    pub fn import_local_files(
        &self,
        cluster_id: ClusterId,
        files: Vec<(PathBuf, ContentType)>,
    ) {
        if files.is_empty() {
            return;
        }

        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            let events = state.services.events.clone();
            match twoclient_content::packages::PackageStore::import_local_files(
                &files,
                cluster_id,
                &state.services.content(),
            )
            .await
            {
                Ok(report) => {
                    let mut deferred = false;
                    for row in &report.imported {
                        let live = twoclient_content::packages::PackageStore::sync_live_content(
                            cluster_id,
                            row,
                            &state.services.content(),
                        )
                        .await
                        .unwrap_or(LiveSync::Skipped);

                        deferred |= live == LiveSync::Deferred;
                    }

                    notify_import(&events, &report, deferred && state.games.is_active(cluster_id));
                    super::invalidate_cluster_queries().await;
                }
                Err(err) => events
                    .notify("Import failed")
                    .body(err.to_string())
                    .error()
                    .send(),
            }
        });
    }

    pub fn install_package(
        &self,
        cluster_id: ClusterId,
        provider: ProviderId,
        project_id: impl Into<String>,
        version_id: impl Into<String>,
    ) {
        let (project_id, version_id) = (project_id.into(), version_id.into());
        let actions = self.clone();

        self.station
            .clone()
            .write_channel(AppChannel::Installs)
            .installs
            .begin(cluster_id, provider, project_id.clone());

        spawn_forever(async move {
            let finish = |actions: &Actions| {
                actions
                    .station
                    .clone()
                    .write_channel(AppChannel::Installs)
                    .installs
                    .finish(cluster_id, provider, &project_id);
            };

            let Ok(state) = launcher::state() else {
                finish(&actions);
                return;
            };

            let install = crate::install::install_package(
                &state,
                provider,
                &project_id,
                &version_id,
                cluster_id,
            )
            .await;

            // Replaces the download's progress notification in place rather
            // than arriving as a second one
            let spec = match &install.result {
                Ok(name) => NotificationSpec {
                    title: "Installed".to_string(),
                    body: crate::install::install_body(
                        name,
                        &install.dependencies,
                        &install.missing_dependencies,
                        install.live_deferred,
                    ),
                    level: Level::Info,
                    icon: Some(IconType::Download01),
                    progress: None,
                    actions: Vec::new(),
                },
                Err(err) => NotificationSpec {
                    title: "Install failed".to_string(),
                    body: err.to_string(),
                    level: Level::Error,
                    icon: None,
                    progress: None,
                    actions: Vec::new(),
                },
            };

            let session_id = install.session_id.unwrap_or_else(uuid::Uuid::nil);
            actions.with_engine(|app| {
                app.notifications
                    .finish_grouped_as_actions(&mut app.inbox, session_id, Some(spec));
            });

            if install.result.is_ok() {
                // Before the refresh below so the version list never renders
                // the moment where both copies read as active
                if let Err(err) = twoclient_content::bundles::reconcile_duplicate_activity(
                    cluster_id,
                    &state.services.content(),
                )
                .await
                {
                    tracing::warn!(%err, "failed to resolve duplicate package versions");
                }

                super::invalidate_cluster_content_queries().await;

                state
                    .services
                    .events
                    .signal(twoclient_events::Signal::ClustersChanged);
            }

            finish(&actions);
        });
    }

    /// Shares the installs channel with [`Self::install_package`] so every row's
    /// button reads as busy while one is being acted on
    ///
    /// Recorded as a bundle override when a bundle owns the artifact otherwise
    /// the next sync puts the version straight back
    pub fn remove_package_version(
        &self,
        cluster_id: ClusterId,
        provider: ProviderId,
        project_id: impl Into<String>,
        hash: impl Into<String>,
        display_name: impl Into<String>,
    ) {
        let (project_id, hash, display_name) =
            (project_id.into(), hash.into(), display_name.into());
        let actions = self.clone();

        self.station
            .clone()
            .write_channel(AppChannel::Installs)
            .installs
            .begin(cluster_id, provider, project_id.clone());

        spawn_forever(async move {
            let finish = |actions: &Actions| {
                actions
                    .station
                    .clone()
                    .write_channel(AppChannel::Installs)
                    .installs
                    .finish(cluster_id, provider, &project_id);
            };

            let Ok(state) = launcher::state() else {
                finish(&actions);
                return;
            };

            let events = state.services.events.clone();
            match twoclient_core::remove_artifact_from_cluster(
                cluster_id,
                &hash,
                true,
                &state.services.content(),
            )
            .await
            {
                Ok(()) => {
                    events.notify("Removed").body(display_name).send();

                    // The row's next state is read off the cluster's content
                    // so refresh that before the busy flag drops
                    super::invalidate_cluster_content_queries().await;
                    events.signal(twoclient_events::Signal::ClustersChanged);
                }
                Err(err) => events
                    .notify("Remove failed")
                    .body(err.to_string())
                    .error()
                    .send(),
            }

            finish(&actions);
        });
    }

    pub fn install_bundle(
        &self,
        cluster_id: ClusterId,
        bundle_name: impl Into<String>,
        skip_compatibility: bool,
    ) {
        let bundle_name = bundle_name.into();
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            let events = state.services.events.clone();
            match twoclient_core::install_bundle(
                cluster_id,
                &bundle_name,
                skip_compatibility,
                state.bundles.as_ref(),
                &state.services.content(),
            )
            .await
            {
                Ok(_) => {
                    events
                        .notify("Installed")
                        .body(format!("Added {bundle_name}"))
                        .send();
                    events.signal(twoclient_events::Signal::ClustersChanged);
                }
                Err(err) => events
                    .notify("Install failed")
                    .body(err.to_string())
                    .error()
                    .send(),
            }
        });
    }

    pub fn apply_bundle_updates(&self, cluster_id: ClusterId) {
        let actions = self.clone();
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            let result = match twoclient_core::apply_bundle_updates(
                cluster_id,
                state.bundles.as_ref(),
                &state.services.content(),
            )
            .await
            {
                Ok(result) => result,
                Err(err) => {
                    state
                        .services
                        .events
                        .notify("Update failed")
                        .body(err.to_string())
                        .error()
                        .send();
                    return;
                }
            };

            super::invalidate_cluster_queries().await;
            if let Some(spec) =
                crate::install::cluster_update_notification(cluster_id, &result, &state.services)
                    .await
            {
                // The offers are queued by the sync itself and raised at launch
                // this path only reports what changed
                actions.push_notification(spec);
            }
        });
    }

    /// Creates an instance and makes it the active one
    ///
    /// The files it needs are left to the first launch to fetch: creating is
    /// a database row and a folder, and making the user watch a download to
    /// find out whether the name took is the wrong trade
    ///
    /// `active` is the root-context selection, which outlives whatever dialog
    /// asked for the instance
    pub fn create_cluster(
        &self,
        options: twoclient_core::CreateClusterOptions,
        dedicated: bool,
        catalog_mods: bool,
        mut active: State<Option<ClusterId>>,
    ) {
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            let global = state.settings.read().global_game_settings.clone();

            let cluster = match state.clusters.create(&global, options).await {
                Ok(cluster) => cluster,
                Err(err) => {
                    state
                        .services
                        .events
                        .notify("Could not create the instance")
                        .body(err.to_string())
                        .error()
                        .send();
                    return;
                }
            };

            // The catalog adds its mods to everything on a matching version and
            // loader, which is not what an instance made by hand is for
            if !catalog_mods
                && let Err(err) = state.clusters.set_follows_catalog(cluster.id, false).await
            {
                tracing::warn!(
                    cluster_id = cluster.id,
                    error = %err,
                    "instance created, but the catalog will still add mods to it"
                );
            }

            // A shared game folder is the launcher's default, which suits the
            // version cards Somebody who names an instance means a separate one
            if dedicated
                && let Err(err) = state
                    .clusters
                    .set_dedicated_dir(cluster.id, true, false)
                    .await
            {
                tracing::warn!(
                    cluster_id = cluster.id,
                    error = %err,
                    "instance created, but it will share the game folder"
                );
            }

            super::invalidate_cluster_queries().await;

            state
                .services
                .events
                .notify("Instance created")
                .body(format!("{} is ready to launch", cluster.name))
                .send();

            *active.write() = Some(cluster.id);
        });
    }

    /// Flips a whole list of packages at once
    ///
    /// One task and one invalidation for the batch: routing each row through
    /// the cluster mutation instead would re-run every cluster query per row,
    /// and the lists this exists for are hundreds of rows long.
    pub fn set_packages_enabled(
        &self,
        cluster_id: ClusterId,
        targets: Vec<crate::components::PackageEntry>,
        enabled: bool,
    ) {
        let actions = self.clone();
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            let content = state.services.content();

            let mut changed = 0usize;
            let mut failed = 0usize;

            for item in packages_to_flip(&targets, enabled) {
                let result = if let Some(hash) = &item.hash {
                    twoclient_core::set_artifact_enabled_to(cluster_id, hash, enabled, &content)
                        .await
                        .map(|_| ())
                } else if let Some(bundle_name) = &item.bundle_name {
                    twoclient_core::set_bundle_package_enabled(
                        cluster_id,
                        bundle_name,
                        &item.package_id,
                        enabled,
                        item.manifest_default,
                        &content,
                    )
                    .await
                } else {
                    continue;
                };

                match result {
                    Ok(()) => changed += 1,
                    Err(err) => {
                        failed += 1;
                        tracing::warn!(
                            cluster_id,
                            package = %item.package_id,
                            error = %err,
                            "bulk enable/disable skipped a package"
                        );
                    }
                }
            }

            super::invalidate_cluster_queries().await;

            let verb = if enabled { "enabled" } else { "disabled" };
            let body = if failed > 0 {
                format!("{changed} {verb}, {failed} could not be changed")
            } else {
                format!("{changed} {verb}")
            };
            actions
                .notify(if enabled { "Packages enabled" } else { "Packages disabled" })
                .body(body)
                .send();
        });
    }

    /// App-scoped on purpose: the copy has to outlive the settings page, which
    /// unmounts as soon as the router swaps to the move screen
    pub fn relocate(&self, plan: RelocationPlan) {
        let state = match launcher::state() {
            Ok(state) => state,
            Err(err) => {
                tracing::error!("move skipped, launcher not ready: {err:#}");
                self.write_relocation(RelocationState {
                    plan: Some(plan),
                    result: Some(Err(format!("TwoClient is not ready yet: {err}"))),
                    ..RelocationState::default()
                });
                return;
            }
        };

        self.write_relocation(RelocationState {
            plan: Some(plan.clone()),
            ..RelocationState::default()
        });

        let mut station = self.station;
        spawn_forever(async move {
            // The copy is on this thread's executor, so it can write the count
            // straight into the channel the move screen reads
            let result = twoclient_core::relocate::relocate(&state, &plan, |copied, total| {
                let mut guard = station.write_channel(AppChannel::Relocation);
                guard.relocation.copied = copied;
                guard.relocation.total = total;
            })
            .await;

            if let Err(message) = &result {
                tracing::error!("moving the data folder failed: {message}");
            }

            let moved = result.is_ok();
            station
                .write_channel(AppChannel::Relocation)
                .relocation
                .result = Some(result);

            if moved {
                super::invalidate_leftovers_queries().await;
            }
        });
    }

    /// Leaves the move screen, which only the user can do and only once the
    /// copy has settled
    pub fn end_relocation(&self) {
        self.write_relocation(RelocationState::default());
    }

    fn write_relocation(&self, relocation: RelocationState) {
        self.station
            .clone()
            .write_channel(AppChannel::Relocation)
            .relocation = relocation;
    }

	/// `syncing_bundles` gates every launch button so the readiness check must
    /// happen before the flag is raised not inside the task
    pub fn sync_bundles(&self) {
        let state = match launcher::state() {
            Ok(state) => state,
            Err(err) => {
                tracing::error!("bundle sync skipped, launcher not ready: {err:#}");
                return;
            }
        };

        let actions = self.clone();
        self.station
            .clone()
            .write_channel(AppChannel::Launcher)
            .launcher
            .syncing_bundles = true;

        spawn_forever(async move {

            let synced = match state.bundles.sync(&state.services.content()).await {
                Ok(_) => true,
                Err(err) => {
                    tracing::error!("bundle catalog sync failed: {err:#}");
                    false
                }
            };
            if let Err(err) = twoclient_core::clusters::apply_remote_migrations(&state).await {
                tracing::error!("cluster migrations failed: {err:#}");
            }
            if let Err(err) = twoclient_core::clusters::ensure_from_bundles(&state).await {
                tracing::error!("bundle cluster provisioning failed: {err:#}");
            }

            // One grouped session for the batch so all downloads surface as a
            // single notification `detach` lets it be converted in place
            let session = twoclient_events::GroupedProgressSession::start(
                &state.services.events,
                "Updating mods",
            );
            let changed = twoclient_content::bundles::sync_all_cluster_bundles(
                state.bundles.as_ref(),
                &state.services.content(),
                Some(&session),
            )
            .await;
            let session_id = session.detach();

            let _ = synced;

            actions
                .station
                .clone()
                .write_channel(AppChannel::Launcher)
                .launcher
                .syncing_bundles = false;
            super::invalidate_cluster_queries().await;

            // Individually installed packages, after the bundles and after the
            // launch button is free again The old home for this was in front of
            // Play where every check was time the player spent watching a
            // spinner
            if let Ok(clusters) = state.clusters.list().await {
                for cluster in clusters {
                    actions.resolve_package_updates(&state, cluster.id).await;
                }
            }

            let spec = crate::install::combined_cluster_update_spec(&changed, &state.services).await;
            actions.with_engine(|app| {
                app.notifications
                    .finish_grouped_as_actions(&mut app.inbox, session_id, spec);
            });
        });
    }

    /// The check runs and the cache is written whatever the mode says keeping
    /// the package manager's "Update available" markers honest
    ///
    /// Runs from the startup sync rather than from a launch Minecraft reads its
    /// mods once at startup so changing them under a running game is not on and
    /// making Play wait for a provider round-trip was the old cost
    async fn resolve_package_updates(
        &self,
        state: &Arc<twoclient_core::LauncherState>,
        cluster_id: ClusterId,
    ) {
        let content = state.services.content();

        let check = match twoclient_core::refresh_browser_package_updates(cluster_id, &content).await
        {
            Ok(check) => check,
            Err(err) => {
                tracing::warn!(
                    cluster_id,
                    error = %err,
                    "browser package update check failed"
                );
                return;
            }
        };

        let pending = check.pending();
        let mode = self.cluster_update_mode(state, cluster_id).await;

        match plan_package_updates(mode, pending) {
            PackageUpdatePlan::Nothing => {}
            PackageUpdatePlan::Apply(updates) => {
                self.apply_package_updates(state, &updates).await;
            }
            PackageUpdatePlan::Prompt(updates) => {
                let Some(group) =
                    crate::install::package_update_group(cluster_id, &updates, &state.services)
                        .await
                else {
                    return;
                };
                self.prompt_package_updates(group).await;
            }
        }
    }

    /// One grouped progress notification so the work is visible while it runs
    async fn apply_package_updates(
        &self,
        state: &Arc<twoclient_core::LauncherState>,
        updates: &[twoclient_core::BrowserPackageUpdate],
    ) {
        let content = state.services.content();
        let session = twoclient_events::GroupedProgressSession::start(
            &state.services.events,
            "Updating packages",
        );
        session.expect(
            twoclient_events::TaskCategory::Packages,
            updates.len() as u64,
            updates.len() as u64,
        );

        let mut applied = 0usize;
        for update in updates {
            let child = session.child(
                update.display_name.clone(),
                1,
                twoclient_events::TaskCategory::Packages,
            );
            match twoclient_core::apply_browser_package_update(update, Some(&child), &content).await
            {
                Ok(_) => applied += 1,
                // The cache row survives so the next check offers it again
                Err(err) => tracing::warn!(
                    package = %update.display_name,
                    error = %err,
                    "automatic package update failed"
                ),
            }
            child.finish();
        }

        let session_id = session.detach();
        let spec = (applied > 0).then(|| NotificationSpec {
            title: "Packages updated".to_string(),
            body: format!(
                "{applied} package{} updated",
                if applied == 1 { "" } else { "s" }
            ),
            level: Level::Info,
            icon: Some(IconType::DownloadCloud02),
            progress: None,
            actions: Vec::new(),
        });

        self.with_engine(|app| {
            app.notifications
                .finish_grouped_as_actions(&mut app.inbox, session_id, spec);
        });

        if applied > 0 {
            super::invalidate_cluster_queries().await;
        }
    }

    async fn prompt_package_updates(&self, group: PackageUpdateGroup) {
        let (done, wait) = tokio::sync::oneshot::channel();

        self.with_engine(move |state| {
            state.notifications.open_package_updates(vec![group], Some(done));
            state.center_open = false;
        });

        let _ = wait.await;
    }

    /// Anything unreadable falls back to the default rather than blocking the
    /// launch on a settings lookup
    async fn cluster_update_mode(
        &self,
        state: &Arc<twoclient_core::LauncherState>,
        cluster_id: ClusterId,
    ) -> PackageUpdateMode {
        let global = state.settings.read().global_game_settings.clone();

        let Ok(cluster) = state.clusters.get(cluster_id).await else {
            return PackageUpdateMode::default();
        };

        state
            .clusters
            .resolve_settings(&global, &cluster)
            .await
            .ok()
            .and_then(|profile| profile.browser_update_mode)
            .unwrap_or_default()
    }

    async fn resolve_optional_mods_before_launch(
        &self,
        state: &Arc<twoclient_core::LauncherState>,
        cluster_id: ClusterId,
    ) {
        let pending = match twoclient_core::pending_optional_mods(
            cluster_id,
            state.bundles.as_ref(),
            &state.services.content(),
        )
        .await
        {
            Ok(pending) => pending,
            Err(err) => {
                tracing::warn!(
                    cluster_id,
                    error = %err,
                    "could not read queued optional mods, launching anyway"
                );
                return;
            }
        };

        // `pending_optional_mods` already drops everything the player turned
        // down, so an empty list here means there is nothing left to ask
        let Some(group) =
            crate::install::pending_optional_group(cluster_id, &pending, &state.services).await
        else {
            return;
        };

        let (done, wait) = tokio::sync::oneshot::channel();
        self.with_engine(move |state| {
            state.notifications.open_optional_mods(vec![group], Some(done));
            state.center_open = false;
        });

        let _ = wait.await;
    }


    pub fn apply_package_update(&self, update: twoclient_core::BrowserPackageUpdate) {
        let actions = self.clone();
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            let events = state.services.events.clone();

            let session = twoclient_events::GroupedProgressSession::start(
                &events,
                format!("Updating {}", update.display_name),
            );
            let child = session.child(
                update.display_name.clone(),
                1,
                twoclient_events::TaskCategory::Packages,
            );

            let result = twoclient_core::apply_browser_package_update(
                &update,
                Some(&child),
                &state.services.content(),
            )
            .await;

            child.finish();
            let session_id = session.detach();

            let spec = match &result {
                Ok(_) => NotificationSpec {
                    title: "Updated".to_string(),
                    body: format!(
                        "{} is now on {}",
                        update.display_name, update.latest_version_name
                    ),
                    level: Level::Info,
                    icon: Some(IconType::DownloadCloud02),
                    progress: None,
                    actions: Vec::new(),
                },
                Err(err) => NotificationSpec {
                    title: "Update failed".to_string(),
                    body: err.to_string(),
                    level: Level::Error,
                    icon: None,
                    progress: None,
                    actions: Vec::new(),
                },
            };

            actions.with_engine(|app| {
                app.notifications
                    .finish_grouped_as_actions(&mut app.inbox, session_id, Some(spec));
            });

            // A failed update stays in the list so the user can retry
            if result.is_ok() {
                actions.with_engine(|app| {
                    app.notifications
                        .resolve_package_update(update.cluster_id, &update.hash);
                });
                super::invalidate_cluster_queries().await;
            }
        });
    }

    /// The package stays marked out of date only the modal stops asking and
    /// only for this version
    pub fn skip_package_update(&self, cluster_id: ClusterId, hash: impl Into<String>) {
        let hash = hash.into();
        let actions = self.clone();
        spawn_forever(async move {
            let Ok(state) = launcher::state() else { return };
            if let Err(err) = twoclient_core::skip_browser_package_update(
                cluster_id,
                &hash,
                &state.services.content(),
            )
            .await
            {
                tracing::warn!(error = %err, "failed to record a skipped package update");
            }

            actions.with_engine(|app| {
                app.notifications.resolve_package_update(cluster_id, &hash);
            });
            super::invalidate_cluster_queries().await;
        });
    }
}

fn group_by_cluster(mods: &[(ClusterId, OptionalModRef)]) -> Vec<(ClusterId, Vec<String>)> {
    let mut grouped: Vec<(ClusterId, Vec<String>)> = Vec::new();

    for (cluster_id, (_bundle_name, package_id)) in mods {
        match grouped.iter_mut().find(|(id, _)| id == cluster_id) {
            Some((_, package_ids)) => package_ids.push(package_id.clone()),
            None => grouped.push((*cluster_id, vec![package_id.clone()])),
        }
    }

    grouped
}

async fn launch(actions: &Actions, cluster_id: ClusterId) {
    let Ok(state) = launcher::state() else { return };
    let events = state.services.events.clone();

    events.game_stage(cluster_id, twoclient_events::LaunchStage::Checking);

    let account = match state.auth.default_account_for_launch().await {
        Ok(account) => account,
        Err(err) => {
            events.game_failed(cluster_id, format!("{err:#}"));
            return;
        }
    };

    let Some(account) = account else {
        events.game_failed(
            cluster_id,
            "Add a Minecraft account before launching.".to_string(),
        );
        return;
    };

    // Play starts the game and nothing else No catalog sync no update check no
    // download the startup sync and the update buttons own that work so a
    // launch costs only what the game itself costs
    actions
        .resolve_optional_mods_before_launch(&state, cluster_id)
        .await;

    if let Err(err) = twoclient_core::launch_cluster(&state, cluster_id, &account, true).await {
        // A missing file is the one failure the launcher can fix itself and a
        // path inside our metadata folder gives the user nothing to act on
        if err.indicates_missing_files() {
            repair_and_relaunch(&state, cluster_id, &account, err).await;
            return;
        }

        events.game_failed(cluster_id, format!("{err:#}"));
    }
}

/// Only ever one retry if the game still will not start after a full rehash and
/// repair the problem is not missing files
async fn repair_and_relaunch(
    state: &std::sync::Arc<twoclient_core::LauncherState>,
    cluster_id: ClusterId,
    account: &twoclient_auth::MinecraftAccount,
    original: twoclient_core::LauncherError,
) {
    let events = state.services.events.clone();

    tracing::warn!(
        cluster_id,
        "launch failed on a missing file; repairing: {original:#}"
    );
    events
        .notify("Repairing installation")
        .body("Some game files are missing. Checking and re-downloading them.")
        .send();

    let report = match twoclient_core::verify_cluster_files(state, cluster_id).await {
        Ok(report) => report,
        Err(repair_err) => {
            tracing::error!(cluster_id, "repair failed: {repair_err:#}");
            // Lead with the original failure the repair failure explains why
            // it was not fixed
            events.game_failed(
                cluster_id,
                format!("{original:#} (repair also failed: {repair_err:#})"),
            );
            return;
        }
    };

    events
        .notify("Repair complete")
        .body(report.summary())
        .send();

    if let Err(err) = twoclient_core::launch_cluster(state, cluster_id, account, true).await {
        tracing::error!(cluster_id, "launch failed again after repair: {err:#}");
        events.game_failed(cluster_id, format!("{err:#}"));
    }
}

fn notify_import(events: &twoclient_events::EventBus, report: &LocalImportReport, deferred: bool) {
    let failed = report.failed.len();

    let Some(mut body) = (match report.imported.as_slice() {
        [] => None,
        [only] => Some(format!("Added {}", only.file_name)),
        rows => Some(format!("Added {} files", rows.len())),
    }) else {
        let reason = report
            .failed
            .first()
            .map_or_else(|| "Nothing could be read".to_string(), |(_, err)| err.to_string());

        events.notify("Import failed").body(reason).error().send();
        return;
    };

    if failed > 0 {
        body.push_str(&format!(", {failed} could not be read"));
    }

    if deferred {
        body.push_str(". Minecraft is running, so it will be there at the next launch");
    }

    if failed == 0 {
        events.notify("Imported").body(body).send();
        return;
    }

    events.notify("Imported").body(body).error().send();
}

#[must_use = "the notification is not raised until `.send()` is called"]
pub struct NotificationBuilder {
    actions: Actions,
    spec: NotificationSpec,
}

impl NotificationBuilder {
    pub fn body(mut self, body: impl Into<String>) -> Self {
        self.spec.body = body.into();
        self
    }

    pub fn level(mut self, level: Level) -> Self {
        self.spec.level = level;
        self
    }

    pub fn info(self) -> Self {
        self.level(Level::Info)
    }

    pub fn error(self) -> Self {
        self.level(Level::Error)
    }

    pub fn icon(mut self, icon: IconType) -> Self {
        self.spec.icon = Some(icon);
        self
    }

    pub fn progress(mut self, current: u64, total: u64) -> Self {
        self.spec.progress = Some((current, total));
        self
    }

    pub fn action(mut self, action: NotificationAction) -> Self {
        self.spec.actions.push(action);
        self
    }

    pub fn actions(mut self, actions: impl IntoIterator<Item = NotificationAction>) -> Self {
        self.spec.actions = actions.into_iter().collect();
        self
    }

    pub fn send(self) {
        self.actions.push_notification(self.spec);
    }
}


#[cfg(test)]
mod tests {
    use super::*;
    use crate::components::PackageEntry;
    use twoclient_content::packages::ProviderId;

    fn row(enabled: bool, hash: Option<&str>, bundle: Option<&str>) -> PackageEntry {
        PackageEntry {
            package_id: "p".into(),
            bundle_name: bundle.map(Into::into),
            provider: ProviderId::Modrinth,
            name: "Sodium".into(),
            file_name: "sodium.jar".into(),
            author: String::new(),
            description: String::new(),
            icon_url: None,
            size: 0,
            categories: Vec::new(),
            enabled,
            installed: hash.is_some(),
            hash: hash.map(Into::into),
            manifest_default: false,
            hidden: false,
            update_available: false,
            seen_status: Default::default(),
        }
    }

    #[test]
    fn a_browser_result_has_nothing_to_flip() {
        let rows = vec![row(false, None, None)];
        assert!(packages_to_flip(&rows, true).is_empty());
        assert!(packages_to_flip(&rows, false).is_empty());
    }

    #[test]
    fn rows_already_in_the_target_state_are_left_alone() {
        let rows = vec![row(true, Some("a"), None), row(false, Some("b"), None)];

        let enabling = packages_to_flip(&rows, true);
        assert_eq!(enabling.len(), 1);
        assert_eq!(enabling[0].hash.as_deref(), Some("b"));

        let disabling = packages_to_flip(&rows, false);
        assert_eq!(disabling.len(), 1);
        assert_eq!(disabling[0].hash.as_deref(), Some("a"));
    }

    #[test]
    fn a_bundle_package_with_no_file_on_disk_is_still_switchable() {
        let rows = vec![row(true, None, Some("Performance"))];
        assert_eq!(packages_to_flip(&rows, false).len(), 1);
    }
}
