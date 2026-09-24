/* SPDX-License-Identifier: MIT */
#include "agent_usage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "model_meta.h"
#include "provider.h"
#include "xalloc.h"
#include "system/clock.h"

void attempt_log_init(struct attempt_log *log)
{
    memset(log, 0, sizeof(*log));
    log->attempt_started_ms = monotonic_ms();
}

void attempt_log_record(struct attempt_log *log, const struct stream_usage *usage,
                        const struct stream_response *response)
{
    long now_ms = monotonic_ms();
    if (log->count == ATTEMPT_LOG_MAX) {
        struct attempt *last = &log->attempts[log->count - 1];
        agent_usage_add(&last->usage, usage);
        last->elapsed_ms += now_ms - log->attempt_started_ms;
    } else {
        struct attempt *attempt = &log->attempts[log->count++];
        attempt->usage = *usage;
        /* A retry backoff cut short by the clock cannot make an attempt shorter than nothing. */
        long elapsed_ms = now_ms - log->attempt_started_ms;
        attempt->elapsed_ms = elapsed_ms > 0 ? elapsed_ms : 0;
        attempt->response_id = response && response->id ? xstrdup(response->id) : NULL;
        attempt->served_model = response && response->model ? xstrdup(response->model) : NULL;
        attempt->route = response && response->route ? xstrdup(response->route) : NULL;
    }
    log->attempt_started_ms = now_ms;
}

void attempt_log_retry(struct attempt_log *log, const struct stream_usage *usage, long delay_ms)
{
    if (usage && agent_usage_is_reported(usage))
        attempt_log_record(log, usage, NULL);
    else
        log->attempt_started_ms = monotonic_ms();
    if (delay_ms > 0)
        log->attempt_started_ms += delay_ms;
}

int attempt_log_has_usage(const struct attempt_log *log)
{
    for (size_t i = 0; i < log->count; i++)
        if (agent_usage_is_reported(&log->attempts[i].usage))
            return 1;
    return 0;
}

void attempt_log_free(struct attempt_log *log)
{
    for (size_t i = 0; i < log->count; i++) {
        free(log->attempts[i].response_id);
        free(log->attempts[i].served_model);
        free(log->attempts[i].route);
    }
    log->count = 0;
}

/* Most providers bill cache writes instead of input; unknown rates use that common policy. */
static long default_uncached_input(const struct stream_usage *usage)
{
    long cached = usage->cached_tokens > 0 ? usage->cached_tokens : 0;
    long cache_write = usage->cache_write_tokens > 0 ? usage->cache_write_tokens : 0;
    long input = usage->input_tokens > 0 ? usage->input_tokens : 0;
    long uncached = input - cached - cache_write;
    return uncached > 0 ? uncached : 0;
}

int agent_usage_is_reported(const struct stream_usage *usage)
{
    return usage->input_tokens >= 0 || usage->output_tokens >= 0 || usage->cost >= 0;
}

static long add_reported(long sum, long extra)
{
    if (extra < 0)
        return sum;
    return (sum < 0 ? 0 : sum) + extra;
}

static int has_unpriced_tokens(const struct stream_usage *usage)
{
    return usage->cost < 0 && (usage->input_tokens >= 0 || usage->output_tokens >= 0);
}

void agent_usage_add(struct stream_usage *sum, const struct stream_usage *extra)
{
    /* Evaluate before the token fields merge below. */
    int unpriced = has_unpriced_tokens(sum) || has_unpriced_tokens(extra);

    sum->input_tokens = add_reported(sum->input_tokens, extra->input_tokens);
    sum->output_tokens = add_reported(sum->output_tokens, extra->output_tokens);
    sum->cached_tokens = add_reported(sum->cached_tokens, extra->cached_tokens);
    sum->cache_write_tokens = add_reported(sum->cache_write_tokens, extra->cache_write_tokens);
    sum->cache_write_1h_tokens =
        add_reported(sum->cache_write_1h_tokens, extra->cache_write_1h_tokens);

    /* An exact cost must cover every token it is summed with; tokens reported without cost
     * make the aggregate unpriceable, so it falls back to the estimated path instead of
     * underreporting an "exact" charge. */
    if (unpriced)
        sum->cost = -1;
    else if (extra->cost >= 0)
        sum->cost = (sum->cost < 0 ? 0 : sum->cost) + extra->cost;
}

struct turn_usage *agent_turn_usage_new(const struct stream_usage *usage, long elapsed_ms,
                                        const struct provider *provider, const char *model)
{
    if (!agent_usage_is_reported(usage) && elapsed_ms < 0)
        return NULL;

    struct turn_usage *turn_usage = xmalloc(sizeof(*turn_usage));
    turn_usage->usage = *usage;
    turn_usage->elapsed_ms = elapsed_ms;
    turn_usage->cost_input = -1;
    turn_usage->cost_cache_read = -1;
    turn_usage->cost_cache_write = -1;
    turn_usage->cost_output = -1;
    turn_usage->cost_total = usage->cost;
    turn_usage->cost_estimated = 0;
    turn_usage->uncached_input_tokens = default_uncached_input(usage);
    turn_usage->provenance = (struct turn_provenance){0};

    /* Without token counts, zero-valued categories would imply a decomposition we do not know. */
    if (usage->input_tokens < 0 && usage->output_tokens < 0)
        return turn_usage;
    if (!model || !*model)
        return turn_usage;

    struct catalog_entry rates;
    if (!model_meta_rates(provider, model, &rates))
        return turn_usage;

    struct catalog_split split;
    double total =
        catalog_price(&rates, usage->input_tokens, usage->output_tokens, usage->cached_tokens,
                      usage->cache_write_tokens, usage->cache_write_1h_tokens, &split);
    if (total < 0)
        return turn_usage;

    turn_usage->uncached_input_tokens = split.uncached_input_tokens;
    turn_usage->cost_input = split.cost_input;
    turn_usage->cost_cache_read = split.cost_cache_read;
    turn_usage->cost_cache_write = split.cost_cache_write;
    turn_usage->cost_output = split.cost_output;
    if (turn_usage->cost_total < 0) {
        turn_usage->cost_total = total;
        turn_usage->cost_estimated = 1;
    }
    return turn_usage;
}
