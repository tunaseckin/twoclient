use freya::query::{Query, QueryCapability, QueryStateData, UseQuery, use_query};
use twoclient_common::domain::GameLoader;
use twoclient_core::{LauncherError, VersionMetadata};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct VersionsMetadataQuery;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct VersionsMetadataKeys;

impl QueryCapability for VersionsMetadataQuery {
    type Ok = Vec<VersionMetadata>;
    type Err = LauncherError;
    type Keys = VersionsMetadataKeys;

    async fn run(&self, _keys: &Self::Keys) -> Result<Self::Ok, Self::Err> {
        let state = crate::launcher::state()?;
        let metadata = state.versions.metadata(&state.services.requester.config().meta_url_base).await;
        if !metadata.is_empty() {
            return Ok(metadata);
        }
        state.versions.sync(&state.services).await?;
        Ok(state.versions.metadata(&state.services.requester.config().meta_url_base).await)
    }
}

pub fn use_versions() -> UseQuery<VersionsMetadataQuery> {
    use_query(Query::new(VersionsMetadataKeys, VersionsMetadataQuery))
}

/// A failed fetch reads as an empty list rather than `None` so callers that
/// block on this don't wait forever when the network is down
pub fn versions_metadata(query: &UseQuery<VersionsMetadataQuery>) -> Option<Vec<VersionMetadata>> {
    let reader = query.read();
    let state = reader.state();
    match &*state {
        QueryStateData::Settled { res: Err(_), .. } => Some(Vec::new()),
        other => other.ok().cloned(),
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct LoaderVersionsQuery;

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct LoaderVersionsKeys {
    pub mc_version: String,
    pub loader: GameLoader,
}

impl QueryCapability for LoaderVersionsQuery {
    type Ok = Vec<String>;
    type Err = LauncherError;
    type Keys = LoaderVersionsKeys;

    async fn run(&self, keys: &Self::Keys) -> Result<Self::Ok, Self::Err> {
        let state = crate::launcher::state()?;
        let mut metadata = state.metadata.lock().await;
        Ok(twoclient_core::get_loader_versions(
            &mut metadata,
            &state.services.mc(),
            &keys.mc_version,
            keys.loader,
        )
        .await?)
    }
}

pub fn use_loader_versions(
    mc_version: String,
    loader: GameLoader,
) -> UseQuery<LoaderVersionsQuery> {
    use_query(Query::new(
        LoaderVersionsKeys { mc_version, loader },
        LoaderVersionsQuery,
    ))
}

pub fn loader_versions(query: &UseQuery<LoaderVersionsQuery>) -> Vec<String> {
    super::state::settled_or_loading(query).unwrap_or_default()
}

/// Every version Mojang publishes, newest first
///
/// Distinct from [`use_versions`], which is the curated list the version cards
/// are built from: creating an instance is not limited to the versions someone
/// wrote cover art for
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct GameVersionsQuery;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct GameVersionsKeys;

impl QueryCapability for GameVersionsQuery {
    type Ok = Vec<twoclient_core::GameVersion>;
    type Err = LauncherError;
    type Keys = GameVersionsKeys;

    async fn run(&self, _keys: &Self::Keys) -> Result<Self::Ok, Self::Err> {
        let state = crate::launcher::state()?;
        let mut metadata = state.metadata.lock().await;
        Ok(twoclient_core::get_game_versions(&mut metadata, &state.services.mc()).await?)
    }
}

pub fn use_game_versions() -> UseQuery<GameVersionsQuery> {
    use_query(Query::new(GameVersionsKeys, GameVersionsQuery))
}

pub fn game_versions(
    query: &UseQuery<GameVersionsQuery>,
) -> Vec<twoclient_core::GameVersion> {
    super::state::settled_or_loading(query).unwrap_or_default()
}

/// Which loaders have a build for one Minecraft version
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct LoadersForVersionQuery;

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct LoadersForVersionKeys {
    pub mc_version: String,
}

impl QueryCapability for LoadersForVersionQuery {
    type Ok = Vec<GameLoader>;
    type Err = LauncherError;
    type Keys = LoadersForVersionKeys;

    async fn run(&self, keys: &Self::Keys) -> Result<Self::Ok, Self::Err> {
        let state = crate::launcher::state()?;
        let mut metadata = state.metadata.lock().await;
        Ok(twoclient_core::get_loaders_for_version(
            &mut metadata,
            &state.services.mc(),
            &keys.mc_version,
        )
        .await?)
    }
}

pub fn use_loaders_for_version(mc_version: String) -> UseQuery<LoadersForVersionQuery> {
    use_query(Query::new(
        LoadersForVersionKeys { mc_version },
        LoadersForVersionQuery,
    ))
}

/// Vanilla is always offered: it needs no loader build to exist
pub fn loaders_for_version(query: &UseQuery<LoadersForVersionQuery>) -> Vec<GameLoader> {
    let mut loaders = super::state::settled_or_loading(query).unwrap_or_default();
    if !loaders.contains(&GameLoader::Vanilla) {
        loaders.insert(0, GameLoader::Vanilla);
    }
    loaders
}
