//! The interactive frontend, and the only place that owns the terminal.
//!
//! reedline reads the line in cooked mode. rxa then enters raw mode for the duration of the
//! turn so a watcher thread can see Esc, which means every write has to carry its own carriage
//! return.

use std::io::Write;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use anyhow::Result;
use crossterm::event::{self, KeyCode, KeyEventKind};
use reedline::{DefaultPrompt, FileBackedHistory, Reedline, Signal};

use super::{Frontend, one_line};
use crate::agent::Agent;
use crate::cancel::Cancel;
use crate::provider::Usage;
use crate::term::RawGuard;

const DIM: &str = "\x1b[2m";
const RED: &str = "\x1b[31m";
const RESET: &str = "\x1b[0m";
const POLL: Duration = Duration::from_millis(80);
const HISTORY_CAPACITY: usize = 1000;

pub struct Repl {
    /// Raw mode swallows the carriage return, so `\n` has to be rewritten on the way out.
    raw: bool,
    line_open: bool,
}

impl Repl {
    pub fn new(raw: bool) -> Self {
        Self {
            raw,
            line_open: false,
        }
    }

    fn put(&mut self, text: &str) {
        let out = if self.raw {
            text.replace('\n', "\r\n")
        } else {
            text.to_string()
        };
        print!("{out}");
        self.line_open = !out.ends_with('\n');
        let _ = std::io::stdout().flush();
    }

    fn line(&mut self, text: &str) {
        if self.line_open {
            self.put("\n");
        }
        self.put(&format!("{text}\n"));
    }
}

impl Frontend for Repl {
    fn text(&mut self, delta: &str) {
        self.put(delta);
    }

    fn tool_start(&mut self, name: &str, arguments: &str) {
        self.line(&format!("{DIM}{name}({}){RESET}", one_line(arguments, 72)));
    }

    fn tool_end(&mut self, _name: &str, body: &str, ok: bool) {
        let colour = if ok { DIM } else { RED };
        self.line(&format!("{colour}  {}{RESET}", one_line(body, 72)));
    }

    fn retry(&mut self, attempt: u32, delay: Duration) {
        self.line(&format!(
            "{DIM}retrying ({attempt}) in {:.1}s{RESET}",
            delay.as_secs_f32()
        ));
    }

    fn turn_end(&mut self, usage: Usage) {
        if usage.total_tokens > 0 {
            self.line(&format!("{DIM}{} tokens{RESET}", usage.total_tokens));
        }
    }

    fn cancelled(&mut self) {
        self.line(&format!("{DIM}cancelled{RESET}"));
    }
}

/// Prompt history survives the session. It cannot be recovered if the file is unusable, so a
/// failure degrades to the in-memory default rather than refusing to start.
fn editor_with_history() -> Reedline {
    let Some(path) = crate::config::history_path() else {
        return Reedline::create();
    };
    match FileBackedHistory::with_file(HISTORY_CAPACITY, path.clone()) {
        Ok(history) => {
            // Prompts are written verbatim, so the file and the directory holding it are
            // owner-only. with_file() has already created both.
            for target in [path.as_path(), path.parent().unwrap_or(&path)] {
                if let Err(e) = crate::config::restrict_to_owner(target) {
                    tracing::warn!("could not restrict {}: {e}", target.display());
                }
            }
            // "/" keeps /quit out of the recall ring; otherwise the first Up in a fresh
            // session hands the user the exit command.
            Reedline::create()
                .with_history(Box::new(history))
                .with_history_exclusion_prefix(Some("/".into()))
        }
        Err(e) => {
            tracing::warn!("history disabled, staying in memory: {e}");
            Reedline::create().with_history_exclusion_prefix(Some("/".into()))
        }
    }
}

/// Runs on the main thread and drives the runtime with `block_on`, so reedline's blocking read
/// never sits inside an async task.
pub fn run(runtime: &tokio::runtime::Runtime, agent: &mut Agent) -> Result<()> {
    let mut editor = editor_with_history();
    let prompt = DefaultPrompt::default();
    let cancel = Cancel::new();

    loop {
        let line = match editor.read_line(&prompt)? {
            Signal::Success(line) => line,
            Signal::CtrlC => continue,
            _ => break,
        };
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }
        if trimmed == "/quit" {
            break;
        }

        let guard = RawGuard::enter()?;
        let stop = Arc::new(AtomicBool::new(false));
        let watcher = watch_for_esc(cancel.clone(), Arc::clone(&stop));

        let mut frontend = Repl::new(true);
        let result = runtime.block_on(agent.run(trimmed, &mut frontend, &cancel));

        stop.store(true, Ordering::SeqCst);
        let _ = watcher.join();
        frontend.line("");
        drop(guard);

        if let Err(e) = result {
            eprintln!("{RED}error: {e:#}{RESET}");
        }
    }
    Ok(())
}

/// A thread, not a task: `crossterm::event::read` blocks, and crossterm's async event stream
/// would be another feature to carry for one key.
fn watch_for_esc(cancel: Cancel, stop: Arc<AtomicBool>) -> std::thread::JoinHandle<()> {
    std::thread::spawn(move || {
        while !stop.load(Ordering::SeqCst) {
            match event::poll(POLL) {
                Ok(true) => {}
                Ok(false) => continue,
                Err(_) => break,
            }
            if let Ok(event::Event::Key(key)) = event::read()
                && key.kind == KeyEventKind::Press
                && key.code == KeyCode::Esc
            {
                cancel.cancel();
            }
        }
    })
}
