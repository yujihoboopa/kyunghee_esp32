#pragma once

#include <stdint.h>

#define GC_PHASE_COUNT 7

typedef enum {
    GC_STEP,
    GC_MARK,
    GC_ATOMIC,
    GC_SWEEP_STRING,
    GC_SWEEP,
    GC_FINALIZE,
    GC_PAUSE
} gc_phase_t;

void gc_probe_record(
    gc_phase_t phase,
    uint64_t elapsed_us
);
void gc_probe_dump_csv(void);
void gc_probe_reset(void);
