//! One wire format: OpenAI-compatible Chat Completions.
//!
//! Because the format is frozen at one, the wire shape *is* the internal shape and there is no
//! translation layer. Adding a second format means introducing one; that is the cost the scope
//! freeze is buying.

pub mod http;
pub mod mock;

use std::pin::Pin;
use std::time::Duration;

use futures_util::Stream;
use serde::{Deserialize, Serialize};

use crate::config::Config;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Role {
    System,
    User,
    Assistant,
    Tool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FunctionCall {
    pub name: String,
    /// A JSON object, as a string. The API streams it in fragments, so it is only parsed once
    /// the turn has assembled it.
    pub arguments: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ToolCall {
    pub id: String,
    #[serde(rename = "type")]
    pub kind: String,
    pub function: FunctionCall,
}

impl ToolCall {
    pub fn function(id: String, name: String, arguments: String) -> Self {
        Self {
            id,
            kind: "function".into(),
            function: FunctionCall { name, arguments },
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Message {
    pub role: Role,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub content: Option<String>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub tool_calls: Vec<ToolCall>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tool_call_id: Option<String>,
}

impl Message {
    pub fn system(text: impl Into<String>) -> Self {
        Self::text(Role::System, text)
    }

    pub fn user(text: impl Into<String>) -> Self {
        Self::text(Role::User, text)
    }

    pub fn assistant(text: Option<String>, tool_calls: Vec<ToolCall>) -> Self {
        Self {
            role: Role::Assistant,
            content: text,
            tool_calls,
            tool_call_id: None,
        }
    }

    pub fn tool_result(call_id: impl Into<String>, body: impl Into<String>) -> Self {
        Self {
            role: Role::Tool,
            content: Some(body.into()),
            tool_calls: Vec::new(),
            tool_call_id: Some(call_id.into()),
        }
    }

    fn text(role: Role, text: impl Into<String>) -> Self {
        Self {
            role,
            content: Some(text.into()),
            tool_calls: Vec::new(),
            tool_call_id: None,
        }
    }
}

#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct Usage {
    #[serde(default)]
    pub prompt_tokens: u32,
    #[serde(default)]
    pub completion_tokens: u32,
    #[serde(default)]
    pub total_tokens: u32,
}

/// Provider-independent by construction, so `turn.rs` never sees wire JSON.
#[derive(Debug, Clone)]
pub enum Event {
    Text(String),
    /// Tool calls arrive interleaved and keyed by index; `id` and `name` land on the first
    /// fragment only.
    ToolCallDelta {
        index: usize,
        id: Option<String>,
        name: Option<String>,
        arguments: Option<String>,
    },
    Usage(Usage),
    Done,
}

pub type EventStream = Pin<Box<dyn Stream<Item = Result<Event, Error>> + Send>>;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("rate limited")]
    RateLimited { retry_after: Option<Duration> },
    #[error("provider returned HTTP {status}")]
    Server { status: u16 },
    #[error("request exceeds the model's context window")]
    ContextExceeded,
    #[error(transparent)]
    Other(#[from] anyhow::Error),
}

impl Error {
    /// Retrying a 4xx other than 429 just burns the budget.
    pub fn is_retryable(&self) -> bool {
        matches!(self, Error::RateLimited { .. } | Error::Server { .. })
    }

    pub fn retry_after(&self) -> Option<Duration> {
        match self {
            Error::RateLimited { retry_after } => *retry_after,
            _ => None,
        }
    }
}

pub enum Provider {
    Http(http::Http),
    Mock(mock::Mock),
}

impl Provider {
    pub async fn stream(
        &self,
        cfg: &Config,
        messages: &[Message],
        tools: &[serde_json::Value],
    ) -> Result<EventStream, Error> {
        match self {
            Provider::Http(p) => p.stream(cfg, messages, tools).await,
            Provider::Mock(p) => p.stream().await,
        }
    }
}
