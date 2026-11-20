// LCS boot movies (RSNY, RSLEEDS, RSNORTH, DM .PSS) played natively: a port of the reference port's src/game/ref_movie.cpp.
//
// lcs_bootstrap's traceMovie replaces the game's movie call (0x35A020, a0 path, a1 skip, a2 frames before a skip is
// allowed) for the four boot movies and calls play(). The game's own MPEG / IPU path stalled after two pictures on
// this runtime. The PSS file is read from the ISO's MOVIES directory (LCS mounts no ps2_disc image), demuxed (MPEG-2
// program stream: video 0xE0; sound in private stream 1, sub-stream 0xFF: "SShd" 16-bit PCM 48 kHz stereo, 512-byte
// interleave, "SSbd", samples - the same format as the reference port), the video decoded with FFmpeg and shown by the presenter at
// 4:3, the sound mixed into the audio output (its played-sample count is the clock). The game thread waits in here.
// Start / Cross skip once the game's threshold allows it. LCS_SKIP_MOVIES=1: skip them all (as before).
#include "lcs_movie.h"

#include "ps2_runtime.h"
#include "runtime/ps2_audio_out.h"
#include "runtime/ps2_pad.h"
#include "runtime/ps2_present.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "runtime/ps2_host_std.h"

#if __has_include(<libavcodec/avcodec.h>)
#define LCS_MOVIES 1
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}
#else
#define LCS_MOVIES 0
#endif

namespace lcs_movie
{
    namespace
    {
        uint32_t le32(const uint8_t *p)
        {
            return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        }

        // the file `name` ("RSNY.PSS;1") of the ISO's MOVIES directory (ISO 9660), empty if missing
        std::vector<uint8_t> readMovie(const std::string &name)
        {
            struct Extent
            {
                uint32_t sector, size;
            };
            std::ifstream disc(PS2Runtime::getIoPaths().cdImage, std::ios::binary);
            auto read = [&](Extent extent) -> std::vector<uint8_t> {
                if (!disc || !extent.size || extent.size > 64u * 1024u * 1024u)
                    return {};
                std::vector<uint8_t> bytes(extent.size);
                disc.seekg(static_cast<std::streamoff>(extent.sector) * 2048);
                if (!disc.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
                    return {};
                return bytes;
            };
            auto find = [&](Extent dir, std::string_view entry, bool directory) -> Extent {
                const auto bytes = read(dir);
                for (size_t i = 0; i < bytes.size();)
                {
                    const size_t length = bytes[i];
                    if (!length)
                    {
                        i = (i / 2048 + 1) * 2048;
                        continue;
                    }
                    if (length < 34 || length > bytes.size() - i || length > 2048 - i % 2048)
                        break;
                    const uint8_t *record = bytes.data() + i;
                    const size_t nameLength = record[32];
                    if (nameLength <= length - 33 && (record[25] & 0x82) == (directory ? 2 : 0) &&
                        std::string_view(reinterpret_cast<const char *>(record + 33), nameLength) == entry)
                        return {le32(record + 2), le32(record + 10)};
                    i += length;
                }
                return {};
            };
            const auto descriptor = read({16, 2048});
            if (descriptor.size() < 190 || descriptor[0] != 1 || std::memcmp(descriptor.data() + 1, "CD001", 5) != 0)
                return {};
            const uint8_t *root = descriptor.data() + 156;
            return read(find(find({le32(root + 2), le32(root + 10)}, "MOVIES", true), name, false));
        }

        // The PSS file, split into its video elementary stream and its sound.
        struct Pss
        {
            std::vector<uint8_t> video;
            std::vector<uint8_t> sound; // the private stream's bytes, sub-header removed (SShd / SSbd / samples)
            double videoStart = 0.0, soundStart = 0.0; // seconds: the first PTS of each
        };

        double readPts(const uint8_t *p)
        {
            const uint64_t pts = (static_cast<uint64_t>((p[0] >> 1) & 7u) << 30) | (static_cast<uint64_t>(p[1]) << 22) |
                                 (static_cast<uint64_t>(p[2] >> 1) << 15) | (static_cast<uint64_t>(p[3]) << 7) | (p[4] >> 1);
            return pts / 90000.0;
        }

        bool demux(const std::vector<uint8_t> &file, Pss &out)
        {
            bool videoPts = false, soundPts = false;
            size_t i = 0;
            const size_t n = file.size();
            while (i + 6 <= n)
            {
                if (file[i] != 0 || file[i + 1] != 0 || file[i + 2] != 1)
                {
                    ++i;
                    continue;
                }
                const uint8_t id = file[i + 3];
                if (id == 0xBA) // pack header (MPEG-2): 14 bytes + stuffing
                {
                    if (i + 14 > n)
                        break;
                    i += 14u + (file[i + 13] & 7u);
                    continue;
                }
                if (id == 0xB9) // end
                    break;
                if (id == 0xBB || id == 0xBD || id == 0xBE || id == 0xBF || (id >= 0xC0 && id <= 0xEF))
                {
                    const size_t length = (static_cast<size_t>(file[i + 4]) << 8) | file[i + 5];
                    const size_t body = i + 6, end = std::min(n, body + length);
                    if (id == 0xE0 || id == 0xBD)
                    {
                        if (body + 3 > end)
                            break;
                        const uint8_t flags = file[body + 1], headerLength = file[body + 2];
                        size_t payload = body + 3 + headerLength;
                        if (payload > end)
                            break;
                        if (id == 0xE0)
                        {
                            if ((flags & 0x80) && !videoPts)
                            {
                                out.videoStart = readPts(&file[body + 3]);
                                videoPts = true;
                            }
                            out.video.insert(out.video.end(), file.begin() + payload, file.begin() + end);
                        }
                        else if (payload + 4 <= end && file[payload] == 0xFF) // sub-stream 0xFF: the sound
                        {
                            if ((flags & 0x80) && !soundPts)
                            {
                                out.soundStart = readPts(&file[body + 3]);
                                soundPts = true;
                            }
                            out.sound.insert(out.sound.end(), file.begin() + payload + 4, file.begin() + end);
                        }
                    }
                    i = end;
                    continue;
                }
                ++i;
            }
            return !out.video.empty();
        }

        // The SShd / SSbd sound as interleaved stereo frames (empty if the format is not the PCM the reference port uses).
        std::vector<int16_t> soundFrames(const std::vector<uint8_t> &sound)
        {
            std::vector<int16_t> out;
            auto u32 = [&](size_t at) {
                uint32_t v;
                std::memcpy(&v, &sound[at], 4);
                return v;
            };
            if (sound.size() < 40 || std::memcmp(sound.data(), "SShd", 4) != 0 || std::memcmp(&sound[32], "SSbd", 4) != 0)
                return out;
            const uint32_t type = u32(8), rate = u32(12), channels = u32(16), interleave = u32(20);
            if (type != 1u || rate != 48000u || channels != 2u || interleave == 0u || interleave % 2u)
            {
                std::fprintf(stderr, "[movie] sound type %u, %u Hz, %u channels, interleave %u: not played\n", type, rate, channels,
                             interleave);
                return out;
            }
            const size_t dataSize = std::min<size_t>(u32(36), sound.size() - 40);
            const uint8_t *data = &sound[40];
            const size_t samplesPerBlock = interleave / 2u;
            const size_t pairs = dataSize / (interleave * 2u);
            out.resize(pairs * samplesPerBlock * 2u);
            for (size_t b = 0; b < pairs; ++b)
            {
                const uint8_t *left = data + b * interleave * 2u, *right = left + interleave;
                for (size_t k = 0; k < samplesPerBlock; ++k)
                {
                    int16_t l, r;
                    std::memcpy(&l, left + k * 2u, 2);
                    std::memcpy(&r, right + k * 2u, 2);
                    out[(b * samplesPerBlock + k) * 2u] = l;
                    out[(b * samplesPerBlock + k) * 2u + 1u] = r;
                }
            }
            return out;
        }

        // Start / Cross on the emulated pad (keyboard, gamepad or injected input; active low)
        bool skipPressed()
        {
            uint8_t pad[32] = {};
            PSPadBackend backend;
            if (!backend.readState(0, 0, pad, sizeof(pad), false, false)) // Start / Cross: no driving remap
                return false;
            const uint16_t buttons = static_cast<uint16_t>(~(pad[2] | (pad[3] << 8)));
            return (buttons & (0x0008u | 0x4000u)) != 0u;
        }

        double nowSeconds()
        {
            return std::chrono::duration<double>(ps2x::hostNow().time_since_epoch()).count();
        }

#if LCS_MOVIES
        // Plays one movie; false if it could not be read or decoded (then the game goes on as if it was skipped).
        bool playFile(const std::string &path, uint32_t skipAfterFrames, PS2Runtime *runtime)
        {
            std::vector<uint8_t> file = readMovie(path);
            if (file.empty())
            {
                std::fprintf(stderr, "[movie] %s not found on the disc\n", path.c_str());
                return false;
            }
            Pss pss;
            if (!demux(file, pss))
                return false;
            std::vector<uint8_t>().swap(file);
            const std::vector<int16_t> sound = soundFrames(pss.sound);

            const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
            AVCodecContext *decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
            AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
            if (!decoder || !parser || avcodec_open2(decoder, codec, nullptr) < 0)
            {
                avcodec_free_context(&decoder);
                if (parser)
                    av_parser_close(parser);
                return false;
            }
            AVPacket *packet = av_packet_alloc();
            AVFrame *frame = av_frame_alloc();
            SwsContext *scaler = nullptr;
            std::vector<uint8_t> rgba;

            // the sound starts the clock (or the host clock without audio output)
            const bool audio = ps2AudioMovieActive() && !sound.empty();
            ps2AudioMovieReset();
            size_t soundPushed = 0;
            const size_t soundFramesTotal = sound.size() / 2u;
            auto pushSound = [&] {
                if (!audio)
                    return;
                while (soundPushed < soundFramesTotal && ps2AudioMovieQueued() < 9600u) // keep ~200 ms queued
                {
                    const uint32_t chunk = static_cast<uint32_t>(std::min<size_t>(2400u, soundFramesTotal - soundPushed));
                    const uint32_t taken = ps2AudioMoviePush(&sound[soundPushed * 2u], chunk);
                    soundPushed += taken;
                    if (taken < chunk)
                        break;
                }
            };
            pushSound();
            const double hostStart = nowSeconds();
            // (the sound may end before the last pictures: from there the host clock carries on where it stopped)
            double soundClock = pss.soundStart, soundClockAt = hostStart;
            auto clock = [&] {
                if (!audio)
                    return pss.videoStart + (nowSeconds() - hostStart);
                if (soundPushed < soundFramesTotal || ps2AudioMovieQueued() > 0u)
                {
                    soundClock = pss.soundStart + ps2AudioMoviePlayed() / 48000.0;
                    soundClockAt = nowSeconds();
                    return soundClock;
                }
                return soundClock + (nowSeconds() - soundClockAt);
            };

            double frameRate = 30000.0 / 1001.0; // the stream's own rate once the decoder knows it
            uint64_t shown = 0, decoded = 0, dropped = 0;
            bool skipHeld = skipPressed(), skipped = false;
            size_t offset = 0;
            bool flushing = false, done = false;
            const double started = nowSeconds();
            while (!done && !runtime->isStopRequested())
            {
                // next decoded picture
                int got = avcodec_receive_frame(decoder, frame);
                if (got == AVERROR(EAGAIN))
                {
                    if (offset < pss.video.size())
                    {
                        uint8_t *data = nullptr;
                        int dataSize = 0;
                        const int used = av_parser_parse2(parser, decoder, &data, &dataSize, pss.video.data() + offset,
                                                          static_cast<int>(std::min<size_t>(4096u, pss.video.size() - offset)),
                                                          AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                        offset += static_cast<size_t>(std::max(used, 0));
                        if (dataSize > 0)
                        {
                            packet->data = data;
                            packet->size = dataSize;
                            avcodec_send_packet(decoder, packet);
                        }
                    }
                    else if (!flushing)
                    {
                        avcodec_send_packet(decoder, nullptr);
                        flushing = true;
                    }
                    else
                        done = true;
                    continue;
                }
                if (got < 0)
                {
                    done = true;
                    continue;
                }
                if (decoder->framerate.num > 0 && decoder->framerate.den > 0)
                    frameRate = av_q2d(decoder->framerate);
                const double due = pss.videoStart + decoded / frameRate;
                ++decoded;
                // late by more than a frame: drop it (keeps the pictures with the sound)
                if (clock() > due + 1.0 / frameRate)
                {
                    ++dropped;
                    continue;
                }
                scaler = sws_getCachedContext(scaler, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                              frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_POINT, nullptr, nullptr, nullptr);
                rgba.resize(static_cast<size_t>(frame->width) * frame->height * 4u);
                uint8_t *planes[1] = {rgba.data()};
                const int strides[1] = {frame->width * 4};
                sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, planes, strides);
                // wait for the picture's moment, feeding the sound and watching for a skip
                while (clock() < due && !runtime->isStopRequested())
                {
                    pushSound();
                    const bool pressed = skipPressed();
                    const bool allowed = (nowSeconds() - started) * 60.0 >= skipAfterFrames; // the game's count, VBlanks
                    if (pressed && !skipHeld && allowed)
                    {
                        skipped = true;
                        break;
                    }
                    skipHeld = pressed;
                    static const bool log = std::getenv("LCS_MOVIE_LOG") != nullptr;
                    static double lastLog = 0.0;
                    if (log && nowSeconds() - lastLog > 1.0)
                    {
                        lastLog = nowSeconds();
                        std::fprintf(stderr, "[movie] %.2f s: pushed %zu, queued %u, played %llu, clock %.2f, due %.2f\n",
                                     lastLog - started, soundPushed, ps2AudioMovieQueued(),
                                     static_cast<unsigned long long>(ps2AudioMoviePlayed()), clock(), due);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                if (skipped)
                    break;
                // the PS2 showed the 512x448 picture across a 4:3 TV
                ps2ShowMovieFrame(rgba.data(), frame->width, frame->height, 4.0f / 3.0f);
                ++shown;
                pushSound();
            }
            // the sound's tail
            while (!skipped && audio && !runtime->isStopRequested() && (soundPushed < soundFramesTotal || ps2AudioMovieQueued()))
            {
                pushSound();
                if (skipPressed() && !skipHeld && (nowSeconds() - started) * 60.0 >= skipAfterFrames)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            ps2AudioMovieReset();
            ps2EndMovieFrames();
            std::fprintf(stderr, "[movie] %s: %llu pictures shown, %llu dropped, %.1f s%s%s\n", path.c_str(),
                         static_cast<unsigned long long>(shown), static_cast<unsigned long long>(dropped), nowSeconds() - started,
                         skipped ? ", skipped" : "", audio ? "" : ", no sound output (host clock)");
            sws_freeContext(scaler);
            av_frame_free(&frame);
            av_packet_free(&packet);
            av_parser_close(parser);
            avcodec_free_context(&decoder);
            return true;
        }
#endif

    }

    bool play(PS2Runtime &runtime, std::string_view name, unsigned skipAfter)
    {
        const std::string path(name);
        if (const char *e = std::getenv("LCS_SKIP_MOVIES"); e && e[0] == '1')
        {
            std::fprintf(stderr, "[movie] %s skipped (LCS_SKIP_MOVIES=1)\n", path.c_str());
            return true;
        }
#if LCS_MOVIES
        if (playFile(path, skipAfter, &runtime))
            return true;
        std::fprintf(stderr, "[movie] %s could not be played: skipped\n", path.c_str());
#else
        (void)runtime;
        (void)skipAfter;
        std::fprintf(stderr, "[movie] %s: built without FFmpeg, skipped\n", path.c_str());
#endif
        return true; // reported as played either way: the game's own movie path stalls on this runtime
    }
    void draw(int) {}
    std::string activeName() { return {}; }
    void shutdown() {}
}
