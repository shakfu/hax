/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_MOCK_H
#define HAX_PROVIDERS_MOCK_H

#include "provider.h"

#ifdef __cplusplus
extern "C" {
#endif

struct provider_def; /* providers/registry.h */

/* Construct the mock provider; providers.mock.script is captured at
 * construction. */
struct provider *mock_provider_new(const struct provider_def *def);

#ifdef __cplusplus
}
#endif

#endif /* HAX_PROVIDERS_MOCK_H */
