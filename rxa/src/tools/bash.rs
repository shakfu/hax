//! Cancellation and the timeout both work by dropping the wait future. `kill_on_drop` then
//! reaps the child, so neither path can leave a process behind.

use std::process::Stdio;
use std::time::Duration;

use anyhow::{Context, Result};
use schemars::JsonSchema;
use serde::Deserialize;
use tokio::process::Command;

use crate::cancel::Cancel;

const DEFAULT_TIMEOUT: Duration = Duration::from_secs(120);
const MAX_TIMEOUT: Duration = Duration::from_secs(600);

#[derive(Debug, Deserialize, JsonSchema)]
pub struct Args {
    /// Shell command to run.
    pub command: String,
    /// Timeout in milliseconds. Defaults to 120000, capped at 600000.
    #[serde(default)]
    pub timeout_ms: Option<u64>,
}

pub async fn call(args: Args, cancel: &Cancel) -> Result<String> {
    let limit = args
        .timeout_ms
        .map_or(DEFAULT_TIMEOUT, Duration::from_millis)
        .min(MAX_TIMEOUT);

    let child = Command::new("bash")
        .arg("-lc")
        .arg(&args.command)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true)
        .spawn()
        .with_context(|| format!("spawning: {}", args.command))?;

    let finished = tokio::select! {
        result = tokio::time::timeout(limit, child.wait_with_output()) => result,
        () = cancel.cancelled() => return Ok("cancelled by the user".into()),
    };

    let Ok(output) = finished else {
        return Ok(format!("timed out after {}ms", limit.as_millis()));
    };
    let output = output.context("waiting for the command")?;

    let mut body = String::new();
    body.push_str(&String::from_utf8_lossy(&output.stdout));
    if !output.stderr.is_empty() {
        body.push_str(&String::from_utf8_lossy(&output.stderr));
    }
    match output.status.code() {
        Some(0) if body.is_empty() => body.push_str("(no output)"),
        Some(0) => {}
        Some(code) => body.push_str(&format!("\n(exit {code})")),
        None => body.push_str("\n(killed by signal)"),
    }
    Ok(body)
}
