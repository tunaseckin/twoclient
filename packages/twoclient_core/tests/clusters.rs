use std::time::Duration;

use twoclient_core::clusters::{ClusterStage, CreateClusterOptions};
use twoclient_common::domain::GameLoader;
use twoclient_common::patch::Patch;
use twoclient_cluster::profiles::create_settings_profile;
use twoclient_core::ProfileUpdate;

#[tokio::test]
async fn cluster_lifecycle_with_settings_profile() {
	let state = twoclient_core::dev::ephemeral_state().await.unwrap();
	let settings = state.settings.read().clone();

	let global = state.settings.read().global_game_settings.clone();

	// Deliberately not a value `default_mem_max()` can return so the assertions
	// distinguish "the cluster's own profile was used" from "fell back to global
	// and matched"
	let cluster = state.clusters.create(
        &global,
        CreateClusterOptions::new("Test Cluster", "1.21.1", GameLoader::Fabric).mem_max(5120),
	)
	.await
	.unwrap();

	assert_eq!(cluster.name, "Test Cluster");
	assert_eq!(cluster.mc_version, "1.21.1");
	assert_eq!(cluster.stage, ClusterStage::NotReady);
	assert!(cluster.setting_profile_name.is_some());
	assert!(cluster.created_at.is_some());

	let resolved = state.clusters.resolve_settings(&global, &cluster)
		.await
		.unwrap();
	assert_eq!(resolved.mem_max, Some(5120));

	state.clusters.update_profile(cluster.id,
		ProfileUpdate {
			mem_max: Patch::Set(6144),
			..Default::default()
		},
	)
	.await
	.unwrap();

	let updated = state.clusters.resolve_settings(&global, &cluster)
		.await
		.unwrap();
	assert_eq!(updated.mem_max, Some(6144));

	let shared = create_settings_profile(&state.services.db, &settings.global_game_settings, "Shared Profile")
		.await
		.unwrap();

	state.clusters.update(cluster.id,
		twoclient_core::ClusterUpdate::default().setting_profile(shared.name.clone()),
	)
	.await
	.unwrap();

	let reassigned = state.clusters.get(cluster.id).await.unwrap();
	assert_eq!(
		reassigned.setting_profile_name.as_deref(),
		Some("Shared Profile")
	);

	let after_play = state.clusters.add_playtime(cluster.id, Duration::from_secs(120))
		.await
		.unwrap();
	assert_eq!(after_play.overall_played, Duration::from_secs(120));
	assert!(after_play.last_played.is_some());

	state.clusters.delete(cluster.id, true).await.unwrap();
	assert!(state.clusters.get(cluster.id).await.is_err());
}

#[tokio::test]
async fn cleared_profile_fields_inherit_from_global() {
	let state = twoclient_core::dev::ephemeral_state().await.unwrap();

	{
		let mut settings = state.settings.write();
		settings.global_game_settings.mem_max = Some(8192);
	}

	// Read the baseline *after* the write above so the new cluster inherits it
	let global = state.settings.read().global_game_settings.clone();
	let cluster = state.clusters.create(
        &global,
        CreateClusterOptions::new("Inherit Test", "1.21.1", GameLoader::Vanilla).mem_max(2048),
	)
	.await
	.unwrap();

	let profile_name = cluster.setting_profile_name.clone().unwrap();

	state.clusters.update_profile(cluster.id,
		ProfileUpdate {
			mem_max: Patch::Clear,
			force_fullscreen: Patch::Clear,
			..Default::default()
		},
	)
	.await
	.unwrap();

    let global = state.settings.read().global_game_settings.clone();
	let stored = twoclient_cluster::profiles::get_profile_or_default(
		&state.services.db,
		&global,
		Some(&profile_name),
	)
	.await
	.unwrap();
	assert_eq!(stored.mem_max, None);
	assert_eq!(stored.force_fullscreen, None);

	let resolved = state.clusters.resolve_settings(&global, &cluster)
		.await
		.unwrap();
	assert_eq!(resolved.mem_max, Some(8192));
	assert_eq!(resolved.force_fullscreen, Some(false));
}

#[tokio::test]
async fn names_are_unique_and_renameable() {
	let state = twoclient_core::dev::ephemeral_state().await.unwrap();
	let global = state.settings.read().global_game_settings.clone();

	let first = state
		.clusters
		.create(
			&global,
			CreateClusterOptions::new("SkyBlock", "1.21.1", GameLoader::Fabric),
		)
		.await
		.unwrap();

	// Same name, and the case it is typed in must not get around the rule
	for attempt in ["SkyBlock", "skyblock"] {
		let clash = state
			.clusters
			.create(
				&global,
				CreateClusterOptions::new(attempt, "1.21.1", GameLoader::Fabric),
			)
			.await;
		assert!(clash.is_err(), "{attempt} was allowed alongside SkyBlock");
	}

	// A different name on the same version and loader is a separate instance
	let second = state
		.clusters
		.create(
			&global,
			CreateClusterOptions::new("SkyBlock Testing", "1.21.1", GameLoader::Fabric),
		)
		.await
		.unwrap();
	assert_ne!(first.id, second.id);
	assert_ne!(first.folder_name, second.folder_name);

	// Renaming onto a taken name is the same clash
	assert!(state.clusters.rename(second.id, "SkyBlock").await.is_err());

	// Renaming to what it is already called is not
	let kept = state.clusters.rename(second.id, "SkyBlock Testing").await;
	assert!(kept.is_ok(), "an instance may keep its own name");

	let renamed = state.clusters.rename(second.id, "Vanilla Survival").await.unwrap();
	assert_eq!(renamed.name, "Vanilla Survival");

	// And the freed name is available again
	assert!(
		state
			.clusters
			.rename(first.id, "SkyBlock Testing")
			.await
			.is_ok()
	);
}

#[tokio::test]
async fn an_instance_can_opt_out_of_the_catalog() {
	let state = twoclient_core::dev::ephemeral_state().await.unwrap();
	let global = state.settings.read().global_game_settings.clone();

	let cluster = state
		.clusters
		.create(
			&global,
			CreateClusterOptions::new("Hand Built", "1.21.1", GameLoader::Fabric),
		)
		.await
		.unwrap();

	// Catalog mods are the default, so a fresh instance is in
	assert!(cluster.follows_catalog());

	state
		.clusters
		.set_follows_catalog(cluster.id, false)
		.await
		.unwrap();
	let out = state.clusters.get(cluster.id).await.unwrap();
	assert!(!out.follows_catalog());

	state
		.clusters
		.set_follows_catalog(cluster.id, true)
		.await
		.unwrap();
	assert!(state.clusters.get(cluster.id).await.unwrap().follows_catalog());
}
