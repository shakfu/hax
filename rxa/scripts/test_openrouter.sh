#!/usr/bin/env bash
# Live test against OpenRouter.
#
#   OPENROUTER_API_KEY=... scripts/test_openrouter.sh
#   MODEL=qwen/qwen3.7-flash scripts/test_openrouter.sh
#
# OpenRouter fronts many upstreams behind one OpenAI-compatible endpoint, so this is the
# broadest single check of what rxa puts on the wire.

PROVIDER=openrouter
BASE_URL=${BASE_URL:-https://openrouter.ai/api/v1}
KEY_ENV=OPENROUTER_API_KEY
# Cheap and reliable at tool calling. ':batch' variants do not stream; ':free' ones rate-limit,
# which would make a failure ambiguous between rxa and the provider.
DEFAULT_MODEL=openai/gpt-5.6-luna

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
run_suite
