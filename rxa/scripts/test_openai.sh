#!/usr/bin/env bash
# Live test against OpenAI.
#
#   OPENAI_API_KEY=... scripts/test_openai.sh
#   MODEL=gpt-5-nano scripts/test_openai.sh
#
# Note that hax routes OpenAI through openai-responses, not Chat Completions. rxa uses
# /chat/completions, which OpenAI still serves; see TODO.md on the dialect trade-off.

PROVIDER=openai
BASE_URL=${BASE_URL:-https://api.openai.com/v1}
KEY_ENV=OPENAI_API_KEY
DEFAULT_MODEL=gpt-5.6-luna

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
run_suite
