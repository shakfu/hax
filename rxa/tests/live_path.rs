//! Behaviour that only appears over the network, so the in-process mock cannot reach it: what
//! rxa actually puts on the wire, and what it does when a gateway serves no model list.

use std::io::{BufRead, BufReader};
use std::process::{Child, Command, Stdio};

/// Kills the fake endpoint however the test ends, including on a failed assertion.
struct Fixture {
    child: Child,
    port: u16,
    dir: tempdir::Dir,
}

impl Fixture {
    fn start(mode: &str) -> Self {
        let dir = tempdir::Dir::new();
        let mut child = Command::new("python3")
            .arg(concat!(
                env!("CARGO_MANIFEST_DIR"),
                "/tests/fixtures/fake_provider.py"
            ))
            .args(["--mode", mode])
            .arg("--capture")
            .arg(dir.path().join("request.json"))
            .arg("--gets")
            .arg(dir.path().join("gets.txt"))
            .stdout(Stdio::piped())
            .spawn()
            .expect("python3 is required for the live-path tests");

        // The fixture prints its ephemeral port once listening, so there is nothing to poll.
        let stdout = child.stdout.take().expect("fixture stdout");
        let mut first = String::new();
        BufReader::new(stdout)
            .read_line(&mut first)
            .expect("fixture never reported a port");
        let port = first
            .trim()
            .strip_prefix("PORT ")
            .and_then(|p| p.parse().ok())
            .unwrap_or_else(|| panic!("unexpected fixture greeting: {first:?}"));

        Self { child, port, dir }
    }

    fn run(&self, extra_env: &[(&str, &str)], prompt: &str) -> std::process::Output {
        let mut cmd = Command::new(env!("CARGO_BIN_EXE_rxa"));
        cmd.env("RXA_BASE_URL", format!("http://127.0.0.1:{}/v1", self.port))
            .env("RXA_API_KEY", "test")
            .env_remove("RXA_MODEL")
            .env_remove("RXA_CONTEXT")
            .args(["-p", prompt]);
        for (k, v) in extra_env {
            cmd.env(k, v);
        }
        cmd.output().expect("running rxa")
    }

    fn captured_request(&self) -> serde_json::Value {
        let raw = std::fs::read(self.dir.path().join("request.json")).expect("no request captured");
        serde_json::from_slice(&raw).expect("captured request was not JSON")
    }

    fn get_paths(&self) -> Vec<String> {
        std::fs::read_to_string(self.dir.path().join("gets.txt"))
            .unwrap_or_default()
            .lines()
            .map(str::to_string)
            .collect()
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

#[test]
fn request_carries_the_cache_key_and_every_tool() {
    let fixture = Fixture::start("full");
    let out = fixture.run(&[], "hello");
    assert!(
        out.status.success(),
        "rxa failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(String::from_utf8_lossy(&out.stdout).contains("live path works"));

    let body = fixture.captured_request();
    let key = body["prompt_cache_key"]
        .as_str()
        .expect("prompt_cache_key missing");
    assert!(key.starts_with("rxa-"), "unexpected cache key: {key}");

    // A mock cannot catch a tool that is never advertised; only the real body can.
    let advertised: Vec<&str> = body["tools"]
        .as_array()
        .expect("no tools advertised")
        .iter()
        .map(|t| t["function"]["name"].as_str().unwrap_or_default())
        .collect();
    assert_eq!(advertised, ["read", "write", "edit", "bash"]);

    // The model and its context window come from the cached /models listing.
    assert_eq!(body["model"], "fake-model");
}

#[test]
fn a_gateway_without_a_model_list_still_runs_when_the_model_is_named() {
    let fixture = Fixture::start("no-models");
    let out = fixture.run(&[("RXA_MODEL", "m"), ("RXA_CONTEXT", "8000")], "hi");

    assert!(
        out.status.success(),
        "rxa failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert!(String::from_utf8_lossy(&out.stdout).contains("live path works"));
    // Nothing was missing, so nothing justified the round-trip.
    assert!(
        fixture.get_paths().is_empty(),
        "unexpected GETs: {:?}",
        fixture.get_paths()
    );
}

#[test]
fn a_gateway_without_a_model_list_fails_clearly_when_the_model_is_not_named() {
    let fixture = Fixture::start("no-models");
    let out = fixture.run(&[], "hi");

    assert!(!out.status.success());
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(stderr.contains("/v1/models"), "unhelpful failure: {stderr}");
}

/// A scratch directory that removes itself, so the fixtures never collide or leak.
mod tempdir {
    use std::path::{Path, PathBuf};

    pub struct Dir(PathBuf);

    impl Dir {
        pub fn new() -> Self {
            let unique = format!(
                "rxa-test-{}-{:?}",
                std::process::id(),
                std::thread::current().id()
            );
            let path = std::env::temp_dir().join(unique.replace(['(', ')', ' '], ""));
            let _ = std::fs::remove_dir_all(&path);
            std::fs::create_dir_all(&path).expect("creating the scratch directory");
            Self(path)
        }

        pub fn path(&self) -> &Path {
            &self.0
        }
    }

    impl Drop for Dir {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
}
