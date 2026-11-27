/* SPDX-License-Identifier: MIT */
/* A C++ host includes hax's headers directly and links against hax's C objects. Compiled as
 * C++17: before C++23, libc++ accepts <atomic> only ahead of <stdatomic.h>, which
 * system/cancel.h includes, so it comes first. The struct sizes C++ sees must match the ones
 * hax_abi() reports from C, or a host would misread every struct it shares with hax. */
#include <atomic>
#include <cstdio>

#include "agent_core.h"
#include "agent_loop.h"
#include "agent_stream.h"
#include "agent_tool.h"
#include "config.h"
#include "diag.h"
#include "hax_embed.h"
#include "provider.h"
#include "tool.h"
#include "system/cancel.h"
#include "tools/output_cap.h"

namespace
{

int failures = 0;

void expect(bool condition, const char *what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

} // namespace

int main()
{
    const struct hax_abi *abi = hax_abi();
    expect(abi->version == HAX_ABI_VERSION, "the ABI version matches");
    expect(abi->sizeof_item == sizeof(struct item), "struct item");
    expect(abi->sizeof_agent_session == sizeof(struct agent_session), "struct agent_session");
    expect(abi->sizeof_agent_loop_params == sizeof(struct agent_loop_params),
           "struct agent_loop_params");
    expect(abi->sizeof_agent_loop_result == sizeof(struct agent_loop_result),
           "struct agent_loop_result");
    expect(abi->sizeof_agent_loop_hooks == sizeof(struct agent_loop_hooks),
           "struct agent_loop_hooks");
    expect(abi->sizeof_tool_def == sizeof(struct tool_def), "struct tool_def");

    /* Calls through the C linkage the guards give these declarations. */
    struct cancel_state cancel = {};
    cancel_state_request_abort(&cancel);
    expect(cancel_state_abort_requested(&cancel) != 0, "cancel state round-trips");
    cancel_state_clear(&cancel);
    expect(cancel_state_abort_requested(&cancel) == 0, "cancel state clears");

    /* One function from each remaining header: each address must resolve to hax's C symbol. */
    void (*const linked[])() = {
        reinterpret_cast<void (*)()>(&agent_stream_init),
        reinterpret_cast<void (*)()>(&agent_tool_call_run),
        reinterpret_cast<void (*)()>(&config_set_override),
        reinterpret_cast<void (*)()>(&hax_warn),
        reinterpret_cast<void (*)()>(&output_cap_clip),
    };
    for (void (*function)() : linked)
        expect(function != nullptr, "a C function links from C++");

    std::atomic<int> host_counter{0};
    host_counter++;
    expect(host_counter.load() == 1, "the host's own atomics work beside hax's");
    return failures ? 1 : 0;
}
