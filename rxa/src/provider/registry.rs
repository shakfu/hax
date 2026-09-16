//! The shipped providers. Pure data: a base URL, a dialect, and where the key comes from.
//!
//! `--base-url` overrides the URL for a local server or a gateway. It never overrides the
//! dialect, because a different wire shape is a different provider, not a different address.

use super::Dialect;

pub struct Entry {
    pub id: &'static str,
    pub base_url: &'static str,
    pub dialect: Dialect,
    /// None for a local server that wants no credential.
    pub key_env: Option<&'static str>,
}

pub const ALL: &[Entry] = &[
    Entry {
        id: "openai",
        base_url: "https://api.openai.com/v1",
        dialect: Dialect::Responses,
        key_env: Some("OPENAI_API_KEY"),
    },
    Entry {
        id: "anthropic",
        base_url: "https://api.anthropic.com/v1",
        dialect: Dialect::Messages,
        key_env: Some("ANTHROPIC_API_KEY"),
    },
    Entry {
        id: "openrouter",
        base_url: "https://openrouter.ai/api/v1",
        dialect: Dialect::Chat,
        key_env: Some("OPENROUTER_API_KEY"),
    },
    Entry {
        id: "ollama",
        base_url: "http://127.0.0.1:11434/v1",
        dialect: Dialect::Chat,
        key_env: None,
    },
    Entry {
        id: "llamacpp",
        base_url: "http://127.0.0.1:8080/v1",
        dialect: Dialect::Chat,
        key_env: None,
    },
];

pub fn find(id: &str) -> Option<&'static Entry> {
    ALL.iter().find(|e| e.id == id)
}

pub fn ids() -> Vec<&'static str> {
    ALL.iter().map(|e| e.id).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_entry_is_findable_and_unique() {
        let mut seen = Vec::new();
        for entry in ALL {
            assert!(!seen.contains(&entry.id), "duplicate provider {}", entry.id);
            seen.push(entry.id);
            assert_eq!(find(entry.id).map(|e| e.id), Some(entry.id));
        }
        assert!(find("nope").is_none());
    }

    /// A remote provider without a key variable would fail with a confusing 401 instead of a
    /// clear message; a local one must not demand a key it has no use for.
    #[test]
    fn remote_entries_name_a_key_and_local_ones_do_not() {
        for entry in ALL {
            let local = entry.base_url.contains("127.0.0.1");
            assert_eq!(entry.key_env.is_none(), local, "{} key/locality", entry.id);
            assert!(entry.base_url.ends_with("/v1"), "{} base url", entry.id);
        }
    }
}
