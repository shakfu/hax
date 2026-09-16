#!/usr/bin/env bash
# Live test against Anthropic, through its OpenAI-compatibility endpoint.
#
#   ANTHROPIC_API_KEY=... scripts/test_anthropic.sh
#   MODEL=claude-sonnet-5 scripts/test_anthropic.sh
#
# rxa does not implement anthropic-messages; that is the deferred amendment in TODO.md. What it
# reaches here is /v1/chat/completions on api.anthropic.com, which accepts bearer auth and the
# OpenAI request shape. Verified present: a keyless POST returns 401, not 404.
#
# Consequence: extended thinking, signatures and explicit cache_control are not available over
# this route. Native Messages is what buys those.

PROVIDER=anthropic
BASE_URL=${BASE_URL:-https://api.anthropic.com/v1}
KEY_ENV=ANTHROPIC_API_KEY
# Cheapest tool-capable Claude. Dated ids are the stable spelling; the undated alias usually
# resolves too, and MODEL overrides either way.
DEFAULT_MODEL=claude-haiku-4-5-20251001

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
run_suite
