//! The continuation loop, shared by both frontends.
//!
//! Presentation and cancellation arrive through `Frontend` and `Cancel`, so the REPL and the
//! headless path cannot drift apart by each growing their own copy of this loop.

use std::time::Duration;

use anyhow::{Result, bail};
use futures_util::StreamExt;

use crate::cancel::Cancel;
use crate::config::{CONTEXT_MARGIN, Config};
use crate::frontend::Frontend;
use crate::provider::{Error, Message, Provider, Usage};
use crate::tools::{self, Tool};
use crate::turn::Assembler;

const MAX_ATTEMPTS: u32 = 4;
const SYSTEM: &str = "You are rxa, a coding agent. Use the tools to inspect and change files. \
Be terse. State what you did; do not narrate what you are about to do.";

pub struct Agent {
    provider: Provider,
    config: Config,
    messages: Vec<Message>,
    tools: Vec<serde_json::Value>,
}

impl Agent {
    pub fn new(provider: Provider, config: Config) -> Self {
        Self {
            provider,
            config,
            messages: vec![Message::system(SYSTEM)],
            tools: tools::specs(),
        }
    }

    /// One user prompt and every turn it spawns, until the model stops asking for tools.
    pub async fn run(
        &mut self,
        prompt: &str,
        frontend: &mut dyn Frontend,
        cancel: &Cancel,
    ) -> Result<()> {
        cancel.reset();
        self.messages.push(Message::user(prompt));

        for _ in 0..self.config.max_turns {
            let Some(turn) = self.one_turn(frontend, cancel).await? else {
                frontend.cancelled();
                return Ok(());
            };

            self.check_context(turn.usage)?;
            frontend.turn_end(turn.usage);
            self.messages.push(Message::assistant(
                (!turn.text.is_empty()).then(|| turn.text.clone()),
                turn.calls.clone(),
            ));

            if !turn.wants_tools() {
                return Ok(());
            }

            for call in &turn.calls {
                if cancel.is_cancelled() {
                    frontend.cancelled();
                    return Ok(());
                }
                let name = call.function.name.as_str();
                frontend.tool_start(name, &call.function.arguments);

                let outcome = match Tool::from_name(name) {
                    Some(tool) => tool.call(&call.function.arguments, cancel).await,
                    None => Err(anyhow::anyhow!("no such tool: {name}")),
                };
                let (ok, body) = match outcome {
                    Ok(body) => (true, body),
                    Err(e) => (false, format!("error: {e:#}")),
                };

                frontend.tool_end(name, &body, ok);
                self.messages
                    .push(Message::tool_result(call.id.clone(), body));
            }
        }

        bail!(
            "stopped after {} turns without a final answer",
            self.config.max_turns
        )
    }

    /// `Ok(None)` means the user cancelled mid-stream.
    async fn one_turn(
        &self,
        frontend: &mut dyn Frontend,
        cancel: &Cancel,
    ) -> Result<Option<crate::turn::Turn>> {
        let mut stream = self.connect(frontend, cancel).await?;
        let mut assembler = Assembler::new();

        loop {
            let next = tokio::select! {
                item = stream.next() => item,
                () = cancel.cancelled() => return Ok(None),
            };
            let Some(item) = next else { break };

            match item? {
                crate::provider::Event::Text(text) => {
                    frontend.text(&text);
                    assembler.push(crate::provider::Event::Text(text));
                }
                event => {
                    assembler.push(event);
                    if assembler.is_done() {
                        break;
                    }
                }
            }
        }

        Ok(Some(assembler.finish()))
    }

    /// Retries the connection only. A stream that dies mid-response is not replayed, because the
    /// partial assistant text has already been shown.
    async fn connect(
        &self,
        frontend: &mut dyn Frontend,
        cancel: &Cancel,
    ) -> Result<crate::provider::EventStream> {
        let mut attempt = 0;
        loop {
            match self
                .provider
                .stream(&self.config, &self.messages, &self.tools)
                .await
            {
                Ok(stream) => return Ok(stream),
                Err(Error::ContextExceeded) => bail!(
                    "the conversation no longer fits in {}'s context window; rxa does not \
                     compact, so start a new session",
                    self.config.model
                ),
                Err(e) if e.is_retryable() && attempt < MAX_ATTEMPTS => {
                    attempt += 1;
                    let delay = e
                        .retry_after()
                        .unwrap_or_else(|| Duration::from_secs(1 << attempt));
                    frontend.retry(attempt, delay);
                    tokio::select! {
                        () = tokio::time::sleep(delay) => {}
                        () = cancel.cancelled() => bail!("cancelled while backing off"),
                    }
                }
                Err(e) => return Err(e.into()),
            }
        }
    }

    /// rxa reports and refuses. Compaction is outside the frozen scope.
    fn check_context(&self, usage: Usage) -> Result<()> {
        let used = usage.total_tokens;
        if used > 0 && used + CONTEXT_MARGIN >= self.config.context {
            bail!(
                "{used} tokens used of {} for {}; start a new session",
                self.config.context,
                self.config.model
            );
        }
        Ok(())
    }
}
