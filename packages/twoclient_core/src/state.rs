use std::sync::Arc;

use twoclient_db::DbPool;
use tokio::sync::Mutex;

use twoclient_auth::AuthService;
use crate::clusters::ClusterManager;
use twoclient_content::bundles::BundlesManager;
use twoclient_discord::DiscordRpc;
use twoclient_net::RequestClient;
use crate::images::ImageCacheStore;
use twoclient_java::JavaService;
use twoclient_events::EventBus;
use twoclient_mc::MetadataStore;
use twoclient_content::packages::provider::PackageProviderRegistry;
use twoclient_common::paths;
use crate::settings::{store, LauncherSettings};
use crate::versions::VersionsManager;
use crate::LauncherResult;

#[derive(Clone)]
pub struct LauncherServices {
	pub events: EventBus,
	pub requester: RequestClient,
	pub db: DbPool,
	pub packages: PackageProviderRegistry,
}

pub struct LauncherState {
	pub services: LauncherServices,
	pub settings: parking_lot::RwLock<LauncherSettings>,
	pub auth: Arc<AuthService>,
	pub java: JavaService,
	pub clusters: ClusterManager,
	pub metadata: Mutex<MetadataStore>,
	pub bundles: Arc<BundlesManager>,
	pub versions: Arc<VersionsManager>,
	pub images: ImageCacheStore,
	pub games: crate::game::GameProcessManager,
	pub discord: DiscordRpc,
}

impl LauncherServices {
	#[must_use]
	pub fn content(&self) -> twoclient_content::ContentCtx {
		twoclient_content::ContentCtx::new(
			self.db.clone(),
			self.requester.clone(),
			self.events.clone(),
			self.packages.clone(),
		)
	}

	/// Both fields are refcounted handles so building this per call is cheap
	#[must_use]
	pub fn mc(&self) -> twoclient_mc::McCtx {
		twoclient_mc::McCtx::new(self.requester.clone(), self.events.clone())
	}
}

impl LauncherState {
	/// No side effects beyond opening the database and reading settings the
	/// background startup work is [`run_startup_tasks`] which the caller runs
    #[tracing::instrument(skip(events))]
	pub async fn new(events: EventBus) -> LauncherResult<Arc<Self>> {
        let services = LauncherServices {
			events,
			db: twoclient_db::connect(paths::database_file()?).await?,
			// Defaults because loading settings needs the event bus already inside
			// `services` Real config is pushed in below before any request
			requester: RequestClient::new(twoclient_net::NetConfig::default())?,
			packages: PackageProviderRegistry::new(),
		};

        let settings = store::load_settings(Some(&services.events)).await;

		twoclient_common::consent::init(settings.declined_tos);
		services
			.requester
			.set_config(crate::settings::net_config(&settings));
        let auth = Arc::new(
			AuthService::load(services.requester.clone(), services.events.clone()).await?,
		);
        let java = JavaService::new(
			std::sync::Arc::new(crate::java_store::SqlJavaStore::new(services.db.clone())),
			services.requester.clone(),
			services.events.clone(),
		);
        let discord = DiscordRpc::spawn(settings.discord_enabled);

		let clusters = ClusterManager::new(services.db.clone());

		let state = Arc::new(Self {
			services,
			settings: parking_lot::RwLock::new(settings),
			auth,
			java,
			clusters,
			metadata: Mutex::new(MetadataStore::new()),
			bundles: Arc::new(BundlesManager::new()),
			versions: Arc::new(VersionsManager::from_cache().await),
			images: ImageCacheStore::new(),
			games: crate::game::GameProcessManager::new(),
			discord,
		});

		Ok(state)
	}
}

/// Java archives are the only downloads big enough for a leaked scratch file to
/// matter and they land flat in the java dir so this needs no recursion
async fn sweep_java_scratch_files() {
	let dir = match paths::java_dir() {
		Ok(dir) => dir,
		Err(err) => {
			tracing::warn!("could not resolve the java dir to sweep: {err:#}");
			return;
		}
	};

	if let Err(err) = polyio::sweep_temp_files(&dir).await {
		tracing::warn!("java scratch file sweep failed: {err:#}");
	}
}

pub fn run_startup_tasks(state: &Arc<LauncherState>) {
	let background = Arc::clone(state);
	tokio::spawn(async move {
			sweep_java_scratch_files().await;

			let recovery = match crate::recovery::reconstruct_from_disk(&background).await {
				Ok(report) => report,
				Err(err) => {
					tracing::error!("disk recovery failed: {err:#}");
					crate::recovery::RecoveryReport::default()
				}
			};

			crate::game::recover_sessions(&background).await;

			let content = background.services.content();
			let (versions_res, bundles_res) = tokio::join!(
				background.versions.sync(&background.services),
				background.bundles.sync(&content),
			);
			if let Err(err) = versions_res {
				tracing::error!("versions manifest sync failed: {err:#}");
			}
			if let Err(err) = bundles_res {
				tracing::error!("bundle catalog sync failed: {err:#}");
			}

			if recovery.did_recover()
				&& let Err(err) = crate::recovery::restore_bundle_tracking(&background).await
			{
				tracing::warn!("bundle tracking restore failed: {err:#}");
			}

			if let Err(err) = crate::clusters::apply_remote_migrations(&background).await {
				tracing::error!("cluster migrations failed: {err:#}");
			}

			if let Err(err) = crate::clusters::ensure_from_versions(&background).await {
				tracing::error!("versions cluster provisioning failed: {err:#}");
			} else {
				background.services.events.signal(twoclient_events::Signal::ClustersChanged);
			}

			// Must run last judging an artifact unused before recovery bundle
			// tracking and provisioning have restored their rows would evict
			// content still in use Only the row-driven half runs unattended
			if let Err(err) =
				twoclient_content::packages::store::collect_unused_artifacts(&content).await
			{
				tracing::warn!("package cache cleanup failed: {err:#}");
			}

		background.services.events.signal(twoclient_events::Signal::SyncComplete);
	});
}
