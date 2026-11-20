// [lcs] the LCS SPU2 output's timing counters (release_spu2/lcs_spu2_output.cpp): the the reference port runtime mixes SPU2 itself;
// zeros until its counters are wired to lcs_timing_trace
#include <cstdint>
void lcsReadAudioTiming(uint64_t (&out)[7]) { for (auto &v : out) v = 0; }
