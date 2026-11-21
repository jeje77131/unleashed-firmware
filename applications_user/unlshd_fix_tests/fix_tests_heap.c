#include "fix_tests.h"

#include <furi.h>

#define REALLOC_OLD_SIZE (16U)
#define REALLOC_NEW_SIZE (48U)
#define REALLOC_FILL     (0x5AU)

// Written twice into the block behind the old one: the grown block holds a run of it when
// realloc() copied from there
static const char realloc_marker[] = "NEIGHBOUR-BLOCK!";
#define REALLOC_MARKER_SIZE  (sizeof(realloc_marker) - 1)
#define REALLOC_MARKER_MATCH (8U)

// Blocks pass through here so the compiler cannot drop an allocation it sees no use for
static void* volatile heap_sink;

void fix_tests_realloc_run(FixTestsReallocResult* result) {
    furi_check(result);

    // Two small blocks in a row: the second usually lands right behind the first, its header
    // and then its data being what comes next in memory
    uint8_t* block = malloc(REALLOC_OLD_SIZE);
    char* neighbour = malloc(2 * REALLOC_MARKER_SIZE);
    memset(block, REALLOC_FILL, REALLOC_OLD_SIZE);
    memcpy(neighbour, realloc_marker, REALLOC_MARKER_SIZE);
    memcpy(neighbour + REALLOC_MARKER_SIZE, realloc_marker, REALLOC_MARKER_SIZE);
    heap_sink = neighbour;

    // malloc() hands out zeroed memory, so anything that is not zero past the first
    // REALLOC_OLD_SIZE bytes was copied from outside the old block
    const volatile uint8_t* grown = realloc(block, REALLOC_NEW_SIZE);
    heap_sink = (void*)grown;

    result->kept = true;
    for(size_t i = 0; i < REALLOC_OLD_SIZE; i++) {
        if(grown[i] != REALLOC_FILL) result->kept = false;
    }

    result->outside = 0;
    for(size_t i = REALLOC_OLD_SIZE; i < REALLOC_NEW_SIZE; i++) {
        if(grown[i] != 0) result->outside++;
    }

    result->neighbour = false;
    for(size_t i = REALLOC_OLD_SIZE; i + REALLOC_MARKER_MATCH <= REALLOC_NEW_SIZE; i++) {
        size_t matched = 0;
        while(matched < REALLOC_MARKER_MATCH &&
              grown[i + matched] == (uint8_t)realloc_marker[matched]) {
            matched++;
        }
        if(matched == REALLOC_MARKER_MATCH) {
            result->neighbour = true;
            break;
        }
    }

    free((void*)grown);
    free(neighbour);
    heap_sink = NULL;
}

bool fix_tests_calloc_run(void) {
    // 1073741825 x 4 is 4 GiB + 4 bytes, which wraps to 4 in 32-bit arithmetic. Volatile, so
    // the compiler neither sees the product nor folds the call away
    volatile size_t count = 0x40000001U;
    volatile size_t size = 4U;

    heap_sink = calloc(count, size);
    // Only reached when calloc() handed something back instead of stopping the firmware
    void* block = heap_sink;
    const bool returned = block != NULL;
    free(block);
    heap_sink = NULL;

    return returned;
}
