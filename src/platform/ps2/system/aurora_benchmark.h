#pragma once
/* AURORA_BENCHMARK_CHAINED_V3_20261010: profiler builds only */
#ifndef AURORA_FRONTEND_PROFILER
#define AURORA_FRONTEND_PROFILER 0
#endif
#if AURORA_FRONTEND_PROFILER
void AuroraBenchmarkBoot(void);
void AuroraBenchmarkTick(void);
#endif
