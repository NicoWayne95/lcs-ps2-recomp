#pragma once
namespace lcs_bootstrap { void setEnabled(bool enabled); void setNativeBootMovie(bool enabled); void setFastIntroTest(bool enabled); bool bootMoviesActive(); bool bootMoviePresenting();
// [lcs] number of frames the game has released to the display (calls of its GS display flip 0x2D9640)
unsigned long long guestFramesPresented();
// [lcs-heap] the game heaps seen so far (main 0x406D50, second 0x634140): address, bytes in use, bytes in all; returns how many
int heapStats(unsigned (*out)[3], int max);
void requestMaverick();
const char* frameRateModeLabel();
void cycleFrameRateMode();
// [lcs] F2 video page: frame-rate mode 0 30, 1 60, 2 120, 3 uncapped; Hor+ factor (display aspect / 4:3)
int frameRateModeIndex();
void setFrameRateMode(int index);
void setHorPlusWidening(float r);
}
