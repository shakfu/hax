//! The network provider. Classifies failures; it does not retry. Retry policy and its indicator
//! live in `agent.rs`, which is the only place that knows whether a user is watching.

use anyhow::anyhow;
use eventsource_stream::Eventsource;
use futures_util::StreamExt;
use serde::Deserialize;
use serde_json::json;
use std::time::Duration;

use super::{Error, Event, EventStream, Message, Usage};
use crate::config::Config;

pub struct Http {
    client: reqwest::Client,
    /// Improves the provider's prompt-cache hit rate across the turns of one conversation, which
    /// matters because the whole transcript is resent every turn. One process is one
    /// conversation today; session resume would supply the session id here instead.
    cache_key: String,
}

impl Http {
    pub fn new() -> anyhow::Result<Self> {
        Ok(Self {
            client: reqwest::Client::builder().build()?,
            cache_key: session_cache_key(),
        })
    }

    pub async fn stream(
        &self,
        cfg: &Config,
        messages: &[Message],
        tools: &[serde_json::Value],
    ) -> Result<EventStream, Error> {
        let mut body = json!({
            "model": cfg.model,
            "messages": messages,
            "stream": true,
            "stream_options": { "include_usage": true },
            "prompt_cache_key": self.cache_key,
        });
        if !tools.is_empty() {
            body["tools"] = json!(tools);
        }

        let url = format!("{}/chat/completions", cfg.base_url);
        tracing::debug!(%url, model = %cfg.model, messages = messages.len(), "request");

        let response = self
            .client
            .post(&url)
            .bearer_auth(&cfg.api_key)
            .json(&body)
            .send()
            .await
            .map_err(|e| Error::Other(anyhow!(e).context(format!("POST {url}"))))?;

        let status = response.status();
        if !status.is_success() {
            return Err(classify(status, response).await);
        }

        let events = response
            .bytes_stream()
            .eventsource()
            .map(|frame| match frame {
                Err(e) => vec![Err(Error::Other(
                    anyhow!(e).context("reading the event stream"),
                ))],
                Ok(frame) if frame.data.trim() == "[DONE]" => vec![Ok(Event::Done)],
                Ok(frame) => match serde_json::from_str::<Chunk>(&frame.data) {
                    Ok(chunk) => chunk.into_events(),
                    Err(e) => {
                        vec![Err(Error::Other(
                            anyhow!(e).context("parsing a stream chunk"),
                        ))]
                    }
                },
            })
            .flat_map(futures_util::stream::iter);

        Ok(Box::pin(events))
    }
}

/// A 400 naming the context window is not a client bug worth a stack trace; it is the one
/// failure the user must be told about in plain words.
async fn classify(status: reqwest::StatusCode, response: reqwest::Response) -> Error {
    let retry_after = response
        .headers()
        .get(reqwest::header::RETRY_AFTER)
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.parse::<u64>().ok())
        .map(Duration::from_secs);

    let code = status.as_u16();
    let body = response.text().await.unwrap_or_default();

    if code == 429 {
        return Error::RateLimited { retry_after };
    }
    if status.is_server_error() {
        return Error::Server { status: code };
    }
    if body.contains("context_length_exceeded") || body.contains("maximum context length") {
        return Error::ContextExceeded;
    }
    Error::Other(anyhow!("{}", body.trim()).context(format!("HTTP {code}")))
}

/// Stable for the process, distinct between runs. No `rand` dependency for one identifier.
fn session_cache_key() -> String {
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_nanos());
    format!("rxa-{:x}-{nanos:x}", std::process::id())
}

#[derive(Deserialize)]
struct Chunk {
    #[serde(default)]
    choices: Vec<Choice>,
    #[serde(default)]
    usage: Option<Usage>,
}

#[derive(Deserialize)]
struct Choice {
    #[serde(default)]
    delta: Delta,
}

#[derive(Deserialize, Default)]
struct Delta {
    #[serde(default)]
    content: Option<String>,
    #[serde(default)]
    tool_calls: Vec<CallDelta>,
}

#[derive(Deserialize)]
struct CallDelta {
    #[serde(default)]
    index: usize,
    #[serde(default)]
    id: Option<String>,
    #[serde(default)]
    function: Option<FnDelta>,
}

#[derive(Deserialize)]
struct FnDelta {
    #[serde(default)]
    name: Option<String>,
    #[serde(default)]
    arguments: Option<String>,
}

impl Chunk {
    fn into_events(self) -> Vec<Result<Event, Error>> {
        let mut out = Vec::new();
        for choice in self.choices {
            if let Some(text) = choice.delta.content.filter(|t| !t.is_empty()) {
                out.push(Ok(Event::Text(text)));
            }
            for call in choice.delta.tool_calls {
                let (name, arguments) = match call.function {
                    Some(f) => (f.name, f.arguments),
                    None => (None, None),
                };
                out.push(Ok(Event::ToolCallDelta {
                    index: call.index,
                    id: call.id,
                    name,
                    arguments,
                }));
            }
        }
        if let Some(usage) = self.usage {
            out.push(Ok(Event::Usage(usage)));
        }
        out
    }
}
