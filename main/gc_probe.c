#include "gc_probe.h"
#include <stdio.h>

typedef struct {
    uint64_t count;
    uint64_t total_us;
    uint64_t max_us;
} gc_phase_stats_t;

static gc_phase_stats_t stats[GC_PHASE_COUNT];
static const char *const names[GC_PHASE_COUNT] = {
    "step", "mark", "atomic", "sweep_string", "sweep", "finalize", "pause"
};

void gc_probe_record(gc_phase_t phase, uint64_t elapsed_us)
{
    if ((unsigned)phase >= GC_PHASE_COUNT) return;
    gc_phase_stats_t *s = &stats[phase];
    s->count++;
    s->total_us += elapsed_us;
    if (elapsed_us > s->max_us) s->max_us = elapsed_us;
}

void gc_probe_reset(void)
{
    for (unsigned i = 0; i < GC_PHASE_COUNT; ++i) stats[i] = (gc_phase_stats_t){0};
}

/* Emit one compact summary per phase; never dump per-step records in real time. */
void gc_probe_dump_csv(void)
{
    for (unsigned i = 0; i < GC_PHASE_COUNT; ++i) {
        const gc_phase_stats_t *s = &stats[i];
        printf("gc_summary,%s,%llu,%llu,%llu,%llu\n",
               names[i], (unsigned long long)s->count,
               (unsigned long long)s->total_us,
               (unsigned long long)(s->count ? s->total_us / s->count : 0),
               (unsigned long long)s->max_us);
    }
}
