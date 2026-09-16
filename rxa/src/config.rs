//! Flags, then environment, then the cache file. No config registry, no config format.

use std::path::PathBuf;

use anyhow::{Result, bail};
use clap::Parser;

/// Tool results are truncated to this many bytes before they enter the context.
pub const TOOL_OUTPUT_CAP: usize = 32 * 1024;

/// Tokens of headroom below the model's context window before the turn is refused.
pub const CONTEXT_MARGIN: u32 = 2048;

#[derive(Parser, Debug, Clone)]
#[command(
    name = "rxa",
    version,
    about = "A minimal agent harness with a frozen feature set"
)]
pub struct Cli {
    /// Headless: answer this prompt, print the result, exit.
    #[arg(short = 'p', long, value_name = "TEXT")]
    pub prompt: Option<String>,

    /// API base for any OpenAI-compatible Chat Completions endpoint.
    #[arg(
        long,
        env = "RXA_BASE_URL",
        default_value = "https://api.openai.com/v1"
    )]
    pub base_url: String,

    #[arg(long, env = "RXA_API_KEY", hide_env_values = true, value_name = "KEY")]
    pub api_key: Option<String>,

    #[arg(long, env = "RXA_MODEL", value_name = "ID")]
    pub model: Option<String>,

    /// Context window in tokens. Falls back to the cached value for the model.
    #[arg(long, env = "RXA_CONTEXT", value_name = "N")]
    pub context: Option<u32>,

    /// Replay a scripted JSON stream instead of calling the network.
    #[arg(long, value_name = "PATH")]
    pub mock: Option<PathBuf>,

    /// Refuse to keep going after this many provider round-trips in one user turn.
    #[arg(long, default_value_t = 32, value_name = "N")]
    pub max_turns: u32,

    /// Re-fetch the model list even if the cache is fresh.
    #[arg(long)]
    pub refresh_models: bool,
}

#[derive(Debug, Clone)]
pub struct Config {
    pub base_url: String,
    pub api_key: String,
    pub model: String,
    pub context: u32,
    pub max_turns: u32,
}

impl Cli {
    /// Resolve into a usable config, consulting the model cache for anything still missing.
    pub async fn resolve(&self) -> Result<Config> {
        if self.mock.is_some() {
            return Ok(Config {
                base_url: self.base_url.clone(),
                api_key: String::new(),
                model: self.model.clone().unwrap_or_else(|| "mock".into()),
                context: self.context.unwrap_or(128_000),
                max_turns: self.max_turns,
            });
        }

        let Some(api_key) = self.api_key.clone() else {
            bail!("no API key: pass --api-key or set RXA_API_KEY");
        };

        let mut cache = crate::cache::Models::load(&self.base_url);
        if self.wants_model_list(cache.is_stale())
            && let Err(e) = cache.refresh(&self.base_url, &api_key).await
        {
            // Plenty of OpenAI-compatible gateways do not serve /models. That must not stop rxa
            // when the user already named the model.
            if self.model.is_none() || self.refresh_models {
                return Err(e);
            }
            tracing::warn!("model list unavailable, continuing: {e:#}");
        }

        let model = match self.model.clone().or_else(|| cache.default_model()) {
            Some(m) => m,
            None => bail!(
                "no model: pass --model or set RXA_MODEL ({} cached)",
                cache.count()
            ),
        };

        Ok(Config {
            base_url: self.base_url.trim_end_matches('/').to_string(),
            context: self
                .context
                .or_else(|| cache.context_for(&model))
                .unwrap_or(128_000),
            api_key,
            model,
            max_turns: self.max_turns,
        })
    }
    /// A round-trip before the first prompt is only justified by a field the flags left unset.
    fn wants_model_list(&self, cache_stale: bool) -> bool {
        self.refresh_models || ((self.model.is_none() || self.context.is_none()) && cache_stale)
    }
}

/// Prompt history. Whatever the user typed goes here verbatim, so it is owner-only.
pub fn history_path() -> Option<PathBuf> {
    config_dir().map(|d| d.join("history.txt"))
}

/// Tighten a path rxa created to owner-only: 0700 for a directory, 0600 for a file. History and
/// the model cache both sit under the config directory, and session resume will land there too.
#[cfg(unix)]
pub fn restrict_to_owner(path: &std::path::Path) -> std::io::Result<()> {
    use std::os::unix::fs::PermissionsExt;

    let metadata = std::fs::metadata(path)?;
    let mode = if metadata.is_dir() { 0o700 } else { 0o600 };
    std::fs::set_permissions(path, std::fs::Permissions::from_mode(mode))
}

#[cfg(not(unix))]
pub fn restrict_to_owner(_path: &std::path::Path) -> std::io::Result<()> {
    Ok(())
}

/// `$XDG_CONFIG_HOME/rxa`, else `$HOME/.config/rxa`. No `dirs` crate for two lines of logic.
pub fn config_dir() -> Option<PathBuf> {
    if let Some(x) = std::env::var_os("XDG_CONFIG_HOME") {
        return Some(PathBuf::from(x).join("rxa"));
    }
    std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config").join("rxa"))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cli(model: Option<&str>, context: Option<u32>, refresh: bool) -> Cli {
        Cli {
            prompt: None,
            base_url: "http://x/v1".into(),
            api_key: Some("k".into()),
            model: model.map(String::from),
            context,
            mock: None,
            max_turns: 32,
            refresh_models: refresh,
        }
    }

    #[test]
    fn fully_specified_run_skips_the_model_list() {
        assert!(!cli(Some("m"), Some(64), false).wants_model_list(true));
    }

    #[test]
    fn a_missing_field_fetches_only_once_the_cache_is_stale() {
        assert!(cli(None, Some(64), false).wants_model_list(true));
        assert!(cli(Some("m"), None, false).wants_model_list(true));
        assert!(!cli(None, Some(64), false).wants_model_list(false));
    }

    #[cfg(unix)]
    #[test]
    fn restrict_to_owner_strips_group_and_other() {
        use std::os::unix::fs::PermissionsExt;

        let dir = std::env::temp_dir().join(format!("rxa-perm-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let file = dir.join("history.txt");
        std::fs::write(&file, b"secret prompt").unwrap();
        std::fs::set_permissions(&file, std::fs::Permissions::from_mode(0o644)).unwrap();

        restrict_to_owner(&file).unwrap();
        restrict_to_owner(&dir).unwrap();

        let mode = |p: &std::path::Path| std::fs::metadata(p).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode(&file), 0o600);
        assert_eq!(mode(&dir), 0o700);
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn an_explicit_refresh_ignores_both() {
        assert!(cli(Some("m"), Some(64), true).wants_model_list(false));
    }
}
