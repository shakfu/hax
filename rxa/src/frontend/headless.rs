//! `-p`: assistant text on stdout, everything else on stderr, so the output pipes cleanly.

use std::io::Write;
use std::time::Duration;

use super::{Frontend, one_line};
use crate::provider::Usage;

pub struct Headless {
    quiet: bool,
}

impl Headless {
    pub fn new(quiet: bool) -> Self {
        Self { quiet }
    }

    fn note(&self, args: std::fmt::Arguments<'_>) {
        if !self.quiet {
            eprintln!("{args}");
        }
    }
}

impl Frontend for Headless {
    fn text(&mut self, delta: &str) {
        print!("{delta}");
        let _ = std::io::stdout().flush();
    }

    fn tool_start(&mut self, name: &str, arguments: &str) {
        self.note(format_args!("[{name}] {}", one_line(arguments, 80)));
    }

    fn tool_end(&mut self, _name: &str, body: &str, ok: bool) {
        if !ok {
            self.note(format_args!("  -> {}", one_line(body, 80)));
        }
    }

    fn retry(&mut self, attempt: u32, delay: Duration) {
        self.note(format_args!(
            "retrying ({attempt}) in {:.1}s",
            delay.as_secs_f32()
        ));
    }

    fn turn_end(&mut self, _usage: Usage) {}

    fn cancelled(&mut self) {
        self.note(format_args!("cancelled"));
    }
}
