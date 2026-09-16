//! Borrowed stream events in, one owned assistant message out. No I/O, no presentation.

use std::collections::BTreeMap;

use crate::provider::{Event, ToolCall, Usage};

#[derive(Debug, Default, Clone)]
pub struct Turn {
    pub text: String,
    pub calls: Vec<ToolCall>,
    pub usage: Usage,
}

impl Turn {
    pub fn wants_tools(&self) -> bool {
        !self.calls.is_empty()
    }
}

#[derive(Debug, Default, Clone)]
struct Partial {
    id: String,
    name: String,
    arguments: String,
}

#[derive(Debug, Default)]
pub struct Assembler {
    text: String,
    /// Keyed by the provider's index, and ordered by it, so calls dispatch in the order the
    /// model emitted them however the fragments interleave.
    calls: BTreeMap<usize, Partial>,
    usage: Usage,
    done: bool,
}

impl Assembler {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn is_done(&self) -> bool {
        self.done
    }

    pub fn push(&mut self, event: Event) {
        match event {
            Event::Text(t) => self.text.push_str(&t),
            Event::Usage(u) => self.usage = u,
            Event::Done => self.done = true,
            Event::ToolCallDelta {
                index,
                id,
                name,
                arguments,
            } => {
                let slot = self.calls.entry(index).or_default();
                if let Some(id) = id {
                    slot.id = id;
                }
                if let Some(name) = name {
                    slot.name = name;
                }
                if let Some(args) = arguments {
                    slot.arguments.push_str(&args);
                }
            }
        }
    }

    pub fn finish(self) -> Turn {
        let calls = self
            .calls
            .into_values()
            .filter(|p| !p.name.is_empty())
            .map(|p| ToolCall::function(p.id, p.name, p.arguments))
            .collect();
        Turn {
            text: self.text,
            calls,
            usage: self.usage,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn delta(index: usize, id: Option<&str>, name: Option<&str>, args: Option<&str>) -> Event {
        Event::ToolCallDelta {
            index,
            id: id.map(String::from),
            name: name.map(String::from),
            arguments: args.map(String::from),
        }
    }

    #[test]
    fn concatenates_text() {
        let mut a = Assembler::new();
        a.push(Event::Text("he".into()));
        a.push(Event::Text("llo".into()));
        assert_eq!(a.finish().text, "hello");
    }

    #[test]
    fn joins_argument_fragments() {
        let mut a = Assembler::new();
        a.push(delta(0, Some("c1"), Some("read"), Some("{\"pa")));
        a.push(delta(0, None, None, Some("th\":\"x\"}")));
        let turn = a.finish();
        assert_eq!(turn.calls.len(), 1);
        assert_eq!(turn.calls[0].id, "c1");
        assert_eq!(turn.calls[0].function.arguments, "{\"path\":\"x\"}");
    }

    #[test]
    fn orders_by_index_not_arrival() {
        let mut a = Assembler::new();
        a.push(delta(1, Some("b"), Some("write"), Some("{}")));
        a.push(delta(0, Some("a"), Some("read"), Some("{}")));
        let turn = a.finish();
        let names: Vec<_> = turn
            .calls
            .iter()
            .map(|c| c.function.name.as_str())
            .collect();
        assert_eq!(names, ["read", "write"]);
    }

    #[test]
    fn drops_calls_that_never_got_a_name() {
        let mut a = Assembler::new();
        a.push(delta(0, Some("c1"), None, Some("{}")));
        assert!(a.finish().calls.is_empty());
    }

    #[test]
    fn records_done_and_usage() {
        let mut a = Assembler::new();
        a.push(Event::Usage(Usage {
            prompt_tokens: 7,
            completion_tokens: 3,
            total_tokens: 10,
        }));
        a.push(Event::Done);
        assert!(a.is_done());
        assert_eq!(a.finish().usage.total_tokens, 10);
    }
}
