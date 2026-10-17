//! Importing from OneClient, the launcher this one was forked from
//!
//! Distinct from [`super::oneclient_v1`], which reads the much older Tauri
//! build. This one shares our schema and folder layout exactly, so detection is
//! a read of the same `clusters` table and importing is a copy of a game
//! directory resolved by the same dedicated-marker rule.

use std::path::{Path, PathBuf};

use sqlx::Row;
use sqlx::sqlite::{SqliteConnectOptions, SqlitePoolOptions};

use crate::LauncherResult;
use twoclient_common::domain::GameLoader;
use twoclient_common::paths;

use super::{ImportTarget, MigrationDetection, MigrationSource, SourceInstance};

/// Content the launcher re-fetches or re-links, so copying it wastes the disk
const IMPORT_EXCLUDE_TOP: &[&str] = &["mods", "logs"];

/// Where OneClient keeps its data
///
/// Two layouts, because it moved: the vendor folder it uses now, and the
/// `org.Polyfrost.OneClient` directory older installs were left in.
fn candidate_roots() -> Vec<PathBuf> {
    if let Ok(dir) = std::env::var("TWOCLIENT_IMPORT_ONECLIENT_DIR") {
        return vec![PathBuf::from(dir)];
    }

    let Some(base) = directories::BaseDirs::new() else {
        return Vec::new();
    };
    let data = base.data_dir();

    vec![
        data.join("Polyfrost").join("OneClient"),
        data.join("org.Polyfrost.OneClient"),
    ]
}

pub fn old_root() -> Option<PathBuf> {
    candidate_roots()
        .into_iter()
        .find(|root| root.join("user_data.db").is_file())
}

#[tracing::instrument]
pub async fn detect() -> LauncherResult<Option<MigrationDetection>> {
    let Some(root) = old_root() else {
        return Ok(None);
    };

    // Our own directory under another name would import us into ourselves
    if paths::data_dir().is_ok_and(|ours| ours == root) {
        return Ok(None);
    }

    let db_path = root.join("user_data.db");
    match detect_inner(&root, &db_path).await {
        Ok(detection) => {
            tracing::info!(
                root = %root.display(),
                instances = detection.instances.len(),
                "oneclient migration: detected install"
            );
            Ok(Some(detection))
        }
        Err(err) => {
            tracing::warn!(
                root = %root.display(),
                error = %err,
                "could not read the OneClient database; skipping its migration"
            );
            Ok(None)
        }
    }
}

/// Mirrors `paths::cluster_game_dir`: a marker file in the cluster folder means
/// the instance kept its own directory, otherwise it shared one
fn source_game_dir(root: &Path, folder_name: &str) -> PathBuf {
    let cluster_dir = root.join("clusters").join(folder_name);
    if cluster_dir.join(paths::DEDICATED_MARKER).exists() {
        cluster_dir
    } else {
        root.join(".minecraft")
    }
}

#[tracing::instrument(level = "debug")]
async fn detect_inner(root: &Path, db_path: &Path) -> LauncherResult<MigrationDetection> {
    let opts = SqliteConnectOptions::new()
        .filename(db_path)
        .read_only(true)
        .immutable(true);
    let pool = SqlitePoolOptions::new()
        .max_connections(1)
        .connect_with(opts)
        .await?;

    let rows = sqlx::query("SELECT id, folder_name, mc_version, mc_loader FROM clusters")
        .fetch_all(&pool)
        .await?;

    let mut instances = Vec::with_capacity(rows.len());
    for row in rows {
        let cluster_id: i64 = row.try_get("id")?;
        let folder_name: String = row.try_get("folder_name")?;
        let mc_version: String = row.try_get("mc_version")?;
        let mc_loader_raw: i64 = row.try_get("mc_loader")?;

        let Some(mc_loader) = GameLoader::from_repr(mc_loader_raw as u8) else {
            tracing::warn!(cluster_id, mc_loader_raw, "unknown loader; skipping instance");
            continue;
        };

        let has_game_dir = polyio::dir_has_content(&source_game_dir(root, &folder_name)).await;

        instances.push(SourceInstance {
            instance_id: cluster_id,
            folder_name,
            mc_version,
            target_mc_version: None,
            mc_loader,
            // Bundles are re-resolved against our own catalog on first launch
            categories: Vec::new(),
            has_game_dir,
        });
    }

    pool.close().await;

    Ok(MigrationDetection {
        source: MigrationSource::OneClient,
        root: root.to_path_buf(),
        instances,
    })
}

#[tracing::instrument(skip(state))]
pub async fn import_game_dir(
    state: &std::sync::Arc<crate::LauncherState>,
    folder_name: &str,
    target: ImportTarget,
) -> LauncherResult<()> {
    let Some(root) = old_root() else {
        return Ok(());
    };

    let src = source_game_dir(&root, folder_name);
    if !src.is_dir() {
        tracing::warn!(folder_name, src = %src.display(), "nothing to import");
        return Ok(());
    }

    let dest = match &target {
        ImportTarget::Shared => paths::shared_minecraft_dir()?,
        ImportTarget::Dedicated { new_cluster_id } => {
            let cluster = state.clusters.get(*new_cluster_id).await?;
            let dir = cluster.dir()?;
            polyio::create_dir_all(&dir).await?;
            polyio::write(cluster.dedicated_marker()?, Vec::new()).await?;
            dir
        }
    };

    polyio::create_dir_all(&dest).await?;
    polyio::copy_dir(&src, &dest, IMPORT_EXCLUDE_TOP).await?;

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or_default();
        let dir = std::env::temp_dir().join(format!("twoclient-{name}-{nanos}"));
        std::fs::create_dir_all(&dir).expect("scratch dir");
        dir
    }

    #[test]
    fn a_dedicated_instance_imports_from_its_own_folder() {
        let root = scratch("dedicated");
        let cluster = root.join("clusters").join("skyblock");
        std::fs::create_dir_all(&cluster).expect("dirs");
        std::fs::write(cluster.join(paths::DEDICATED_MARKER), b"").expect("marker");

        assert_eq!(source_game_dir(&root, "skyblock"), cluster);

        std::fs::remove_dir_all(&root).ok();
    }

    #[test]
    fn a_shared_instance_imports_from_the_shared_minecraft() {
        let root = scratch("shared");
        std::fs::create_dir_all(root.join("clusters").join("vanilla")).expect("dirs");

        assert_eq!(source_game_dir(&root, "vanilla"), root.join(".minecraft"));

        std::fs::remove_dir_all(&root).ok();
    }
}
