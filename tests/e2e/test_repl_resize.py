#!/usr/bin/env python3
"""Repainting after a resize. The prompt and pickers notice a resize on the next key; by then the
terminal has rewrapped what they painted, and the repaint must start on the right row so it leaves
no stale rows behind."""

import json
import time

import harness


def test_prompt_rewraps():
    """A prompt whose rows rewrap repaints cleanly after the terminal narrows."""
    term = harness.Terminal("")
    term.wait_for("❯")
    term.type(
        "alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima mike "
        "november oscar papa quebec romeo sierra tango"
    )
    term.wait_for("tango")

    term.resize(30, 24)
    term.type("X")
    # tmux keeps the cursor row as the prompt's rows rewrap, scrolling the banner away.
    term.expect_screen(
        """
        ❯ alpha bravo charlie delta
          echo foxtrot golf hotel
          india juliet kilo lima mike
          november oscar papa quebec
          romeo sierra tangoX
        """
    )


def test_caret_at_row_end_survives_exact_width_resize():
    """A narrowing to exactly the width of the caret's row leaves the rows above the prompt intact."""
    term = harness.Terminal("")
    term.wait_for("❯")
    # After the 2-cell prompt, these 28 characters fill 30 cells with the caret just past them.
    term.type("abcdefghijklmnopqrstuvwxyz12")
    term.wait_for("z12")

    term.resize(30, 24)
    term.send("C-a")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ❯ abcdefghijklmnopqrstuvwxyz1
          2
        """
    )


def test_picker_rewraps():
    """A picker whose rows rewrap repaints cleanly after the terminal narrows, and cancelling it
    erases it."""
    home, workdir = harness.make_home()
    config_dir = home / ".config" / "hax"
    config_dir.mkdir(parents=True)
    # Rows that fit 80 columns but rewrap at 40, so the repaint must climb past the extra rows.
    presets = {
        name: {"provider": "mock", "model": f"{name}-model-with-a-deliberately-long-name"}
        for name in ("alpha", "bravo", "charlie")
    }
    (config_dir / "config.json").write_text(json.dumps({"presets": presets}))
    term = harness.Terminal("", home=home, workdir=workdir)
    term.wait_for("❯")
    term.type("/preset")
    term.send("Enter")
    term.wait_for("charlie")

    term.resize(40, 24)
    term.send("Down")
    # tmux keeps the cursor row as the picker's rows rewrap, scrolling the banner away.
    term.expect_screen(
        """
        ▌ /preset

        select a preset

        ⌕ type to search 3 items

          alpha  mock · alpha-model-with-a-deli…
        → bravo  mock · bravo-model-with-a-deli…
          charlie  mock · charlie-model-with-a-…
        """
    )

    term.send("Escape")
    term.expect_screen(
        """
        ▌ /preset

        ❯
        """
    )


def test_status_row_survives_exact_width_resize():
    """A narrowing to exactly the status row's width leaves the conversation above it intact."""
    term = harness.Terminal(
        """
        delay 30000
        text Never delivered.
        end-turn
        """
    )
    term.wait_for("❯")
    term.type("go")
    term.send("Enter")
    term.wait_for("working...")

    # The 12-cell status row exactly fills a 12-column row, where tmux keeps the cursor after it.
    for _ in range(2):
        term.resize(12, 24)
        # The footer repaints on its animation timer, not on a key; let frames land at each width.
        time.sleep(0.3)
        term.resize(80, 24)
        time.sleep(0.3)
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ go

        {*} working...
        """
    )


if __name__ == "__main__":
    harness.main(globals())
