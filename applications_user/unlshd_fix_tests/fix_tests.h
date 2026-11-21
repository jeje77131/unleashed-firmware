/**
 * @file fix_tests.h
 * Fix Tests 095: on-device checks for defects 1, 3 and 5 of the unlshd-095 review, which
 * cannot be seen in normal use. Each one reports BUG on unpatched firmware and OK on patched.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Service the defect 3 race runs against */
typedef enum {
    FixTestsRaceGps,
    FixTestsRaceNetwork,
} FixTestsRaceTarget;

/** Outcome of the defect 3 race */
typedef struct {
    uint32_t rounds; /**< Subscribe/unsubscribe rounds run */
    uint32_t calls; /**< Callback calls seen */
    uint32_t late; /**< Calls still running, or started, once unsubscribing had returned */
} FixTestsRaceResult;

/** Outcome of the defect 5 realloc check */
typedef struct {
    uint32_t outside; /**< Non-zero bytes past the old block's end in the grown block */
    bool neighbour; /**< Data of the block behind the old one turned up in the grown block */
    bool kept; /**< The old block's own bytes came through */
} FixTestsReallocResult;

/** Defect 3: subscribe and unsubscribe a callback while the service keeps calling it
 *
 * A local RPC session stands in for the companion app and feeds the service, so the calls
 * take the same path as a fix or an event from the phone. Takes about two seconds.
 *
 * @param      target  service to run against
 * @param[out] result  outcome
 */
void fix_tests_race_run(FixTestsRaceTarget target, FixTestsRaceResult* result);

/** Defect 5: grow a small block with realloc() and look at what came along
 *
 * @param[out] result  outcome
 */
void fix_tests_realloc_run(FixTestsReallocResult* result);

/** Defect 5: ask calloc() for 1073741825 elements of 4 bytes
 *
 * Patched firmware stops in calloc() with "out of memory", as it does for any allocation it
 * cannot serve, and the device reboots: this function does not return there.
 *
 * @return     true when calloc() handed back a block (the product wrapped to 4 bytes)
 */
bool fix_tests_calloc_run(void);
