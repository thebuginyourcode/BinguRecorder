#include "Recorder.hpp"
#include "AviWriter.hpp"
#include "ImaAdpcm.hpp"
#include "JpegEncoder.hpp"

#include <CTRPluginFramework.hpp>
#include <atomic>
#include <cstring>
#include <ctime>
#include <string>

namespace Bingu
{
    using namespace CTRPluginFramework;

    static const char  *OutputDir = "/BinguRecorder";
    static const char  *SettingsPath = "/BinguRecorder/settings.bin";

    // Stay well under the 2 GiB limit a lot of AVI players have; longer
    // recordings continue in a new "_partN" file.
    static constexpr u32    MaxFileSize = 2000000000u;

    // Frame buffers between the game thread and the worker threads
    static constexpr u32    SlotCount = 3;

    enum State : u32
    {
        Idle,
        Recording,
        Stopping,
    };

    struct ScreenInfo
    {
        u32     address{0};
        u32     stride{0};
        u32     format{0};
    };

    Settings                Recorder::settings;

    static u8              *g_slots[SlotCount];
    static u32              g_slotSize = 0;
    static u32              g_slotFrame[SlotCount];
    static std::atomic<u32> g_head{0};  ///< Frames captured (written by the game thread)
    static std::atomic<u32> g_tail{0};  ///< Frames saved (written by the worker threads)
    static std::atomic<u32> g_state{Idle};
    static std::atomic<bool> g_failed{false};

    static LightLock        g_lock;     ///< Guards the recording session against the capture hook
    static LightEvent       g_wake;     ///< Wakes the worker threads
    static LightEvent       g_idle;     ///< Signaled once a recording is fully saved

    // Session parameters, set by Start() while holding g_lock
    static CaptureScreens   g_screens;
    static bool             g_half;
    static u32              g_fps;
    static u32              g_width;
    static u32              g_height;
    static u64              g_startTick;
    static u64              g_pauseTick;    ///< When the game was paused, 0 if running
    static bool             g_paused = false;
    static u32              g_nextFrame;
    static u32              g_stopFrame;
    static ScreenInfo       g_bottom;

    // Numbers for the .txt report saved next to each video, to see where the
    // time goes on a real console. Approximate: updated without locking.
    struct Stats
    {
        u32         gameFrames;         ///< Top screen frames the game showed
        u32         captured;
        u32         skippedBusy;        ///< Due, but every buffer was still being saved
        u32         lastBusyFrame;
        u32         encoded[2];         ///< Per worker
        u64         encodeTicks[2];
        u32         tooBig;             ///< JPEG didn't fit the buffer
        u32         audioTicks;
        u32         audioMissedTicks;
        u32         audioLostSamples;
        const char *audioStatus;
    };

    static Stats            g_stats;

    //--------------------------------------------------------------------------
    // Settings
    //--------------------------------------------------------------------------

    void    Settings::Load(void)
    {
        File file;
        Settings loaded;

        if (File::Open(file, SettingsPath, File::READ) != File::SUCCESS)
            return;

        if (file.Read(&loaded, sizeof(loaded)) == File::SUCCESS && loaded.version == version)
        {
            if (loaded.screens <= (u8)CaptureScreens::Both && loaded.fps >= 1 && loaded.fps <= 30
                && loaded.quality >= 1 && loaded.quality <= 100 && loaded.audio <= 2 && loaded.priority <= 1)
                *this = loaded;
        }
        file.Close();
    }

    void    Settings::Save(void) const
    {
        File file;

        if (!Directory::IsExists(OutputDir))
            Directory::Create(OutputDir);

        if (File::Open(file, SettingsPath, File::RWC | File::TRUNCATE) != File::SUCCESS)
            return;
        file.Write(this, sizeof(*this));
        file.Close();
    }

    //--------------------------------------------------------------------------
    // Pixel conversion: 3DS framebuffer -> RGB555 bottom-up bitmap
    //--------------------------------------------------------------------------

    // 3DS framebuffers are stored rotated: one column of 240 pixels after the
    // other, each column starting with the bottom pixel. That's conveniently
    // the same row order as a bottom-up BMP, so source row r == bitmap row r.
    template <u32 Format>
    static inline u16   ToRGB555(const u8 *p)
    {
        switch (Format)
        {
        case GSP_RGBA8_OES: // memory order: A B G R
            return ((p[3] >> 3) << 10) | ((p[2] >> 3) << 5) | (p[1] >> 3);
        case GSP_BGR8_OES:  // memory order: B G R
            return ((p[2] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[0] >> 3);
        case GSP_RGB565_OES:
        {
            u16 c = *reinterpret_cast<const u16 *>(p);
            return ((c >> 1) & 0x7FE0) | (c & 0x1F);
        }
        case GSP_RGB5_A1_OES:
            return *reinterpret_cast<const u16 *>(p) >> 1;
        default:            // GSP_RGBA4_OES
        {
            u16 c = *reinterpret_cast<const u16 *>(p);
            return (((c >> 12) & 0xF) << 11) | (((c >> 8) & 0xF) << 6) | (((c >> 4) & 0xF) << 1);
        }
        }
    }

    template <u32 Format, u32 Bpp>
    static void     CopyColumns(u16 *dst, u32 dstWidth, const u8 *src, u32 stride, u32 columns, u32 step)
    {
        const u32 rows = 240 / step;
        u32 column[240 * 4 / sizeof(u32)];

        for (u32 x = 0; x < columns; ++x, src += stride * step, ++dst)
        {
            // The framebuffer is mapped uncached: one bulk copy of the column
            // is much faster than reading it pixel by pixel
            std::memcpy(column, src, 240 * Bpp);

            const u8 *in = reinterpret_cast<const u8 *>(column);
            u16 *out = dst;

            for (u32 r = 0; r < rows; ++r, in += Bpp * step, out += dstWidth)
                *out = ToRGB555<Format>(in);
        }
    }

    // Copies one screen into the frame. `dstX`/`dstRow` are where the
    // screen's bottom-left pixel lands in the bottom-up bitmap.
    static void     CopyScreen(u16 *frame, u32 dstX, u32 dstRow, const ScreenInfo &screen, u32 screenWidth)
    {
        const u32 step = g_half ? 2 : 1;
        const u32 columns = screenWidth / step;
        const u8 *src = reinterpret_cast<const u8 *>(screen.address);
        u16 *dst = frame + dstRow * g_width + dstX;

        // CTRPF gives us an uncached alias of the framebuffer in physical
        // memory (0 if the game's framebuffer couldn't be converted). It is
        // not part of the game's memory map, so Process::CheckAddress()
        // would reject it.
        if (!src)
            return;

        switch (screen.format & 7)
        {
        case GSP_RGBA8_OES:   CopyColumns<GSP_RGBA8_OES, 4>(dst, g_width, src, screen.stride, columns, step); break;
        case GSP_BGR8_OES:    CopyColumns<GSP_BGR8_OES, 3>(dst, g_width, src, screen.stride, columns, step); break;
        case GSP_RGB565_OES:  CopyColumns<GSP_RGB565_OES, 2>(dst, g_width, src, screen.stride, columns, step); break;
        case GSP_RGB5_A1_OES: CopyColumns<GSP_RGB5_A1_OES, 2>(dst, g_width, src, screen.stride, columns, step); break;
        default:              CopyColumns<GSP_RGBA4_OES, 2>(dst, g_width, src, screen.stride, columns, step); break;
        }
    }

    //--------------------------------------------------------------------------
    // Capture (runs on the game's thread for every presented frame)
    //--------------------------------------------------------------------------

    static u32      ElapsedFrames(u64 now)
    {
        return (u32)((now - g_startTick) * g_fps / SYSCLOCK_ARM11);
    }

    static void     CaptureFrame(const Screen &top)
    {
        u32 slot = g_head % SlotCount;
        u16 *frame = reinterpret_cast<u16 *>(g_slots[slot] + AviWriter::ChunkHeaderSize);
        const u32 div = g_half ? 2 : 1;

        ScreenInfo topInfo;
        topInfo.address = top.LeftFramebuffer;
        topInfo.stride = top.Stride;
        topInfo.format = top.Format;

        switch (g_screens)
        {
        case CaptureScreens::Top:
            CopyScreen(frame, 0, 0, topInfo, 400);
            break;
        case CaptureScreens::Bottom:
            CopyScreen(frame, 0, 0, g_bottom, 320);
            break;
        case CaptureScreens::Both:
            // Top screen above, bottom screen centered below it
            CopyScreen(frame, 0, 240 / div, topInfo, 400);
            CopyScreen(frame, 40 / div, 0, g_bottom, 320);
            break;
        }
    }

    static void     DrawIndicator(const Screen &screen)
    {
        u32 seconds = (u32)((svcGetSystemTick() - g_startTick) / SYSCLOCK_ARM11);

        if (seconds & 1)
            screen.DrawRect(6, 7, 6, 6, Color::Red);
        screen.Draw(Utils::Format("REC %02u:%02u", seconds / 60, seconds % 60), 16, 5, Color::White, Color(0, 0, 0, 0));
    }

    static bool     OnScreenFrame(const Screen &screen)
    {
        if (g_state != Recording)
            return false;

        LightLock_Lock(&g_lock);

        bool drew = false;

        if (g_state == Recording)
        {
            if (!screen.IsTop)
            {
                // The bottom screen is read when the top screen is presented:
                // a lot of games only redraw the bottom screen occasionally.
                g_bottom.address = screen.LeftFramebuffer;
                g_bottom.stride = screen.Stride;
                g_bottom.format = screen.Format;

                if (g_screens == CaptureScreens::Top && Recorder::settings.indicator)
                {
                    DrawIndicator(screen);
                    drew = true;
                }
            }
            else
            {
                u32 frameNo = ElapsedFrames(svcGetSystemTick());

                // Due for a new frame, and the writer has a free buffer?
                // If not, this frame is skipped and shows up as a repeat.
                ++g_stats.gameFrames;

                if (frameNo >= g_nextFrame && g_head - g_tail < SlotCount)
                {
                    CaptureFrame(screen);
                    g_slotFrame[g_head % SlotCount] = frameNo;
                    g_nextFrame = frameNo + 1;
                    ++g_head;
                    ++g_stats.captured;
                    LightEvent_Signal(&g_wake);
                }
                else if (frameNo >= g_nextFrame && frameNo != g_stats.lastBusyFrame)
                {
                    ++g_stats.skippedBusy;
                    g_stats.lastBusyFrame = frameNo;
                }

                if (g_screens == CaptureScreens::Bottom && Recorder::settings.indicator)
                {
                    DrawIndicator(screen);
                    drew = true;
                }
            }
        }

        LightLock_Unlock(&g_lock);
        return drew;
    }

    //--------------------------------------------------------------------------
    // Audio capture
    //
    // The DSP writes the final mix (what comes out of the speakers) into its
    // shared memory: 160 stereo samples per audio tick, at 32728 Hz, so about
    // every 4.9 ms. That memory exists twice, as a double buffer. The copy
    // with the higher frame counter holds the newest samples until the game
    // submits the next tick. A small thread checks the counters every 2 ms
    // and copies each new tick into a ring buffer.
    //--------------------------------------------------------------------------

    static constexpr u32    DspRegions[2] = { 0x1FF50000, 0x1FF70000 };
    static constexpr u32    DspFinalSamples = (0x8540 - 0x8000) * 2;   ///< DSP word address -> byte offset
    static constexpr u32    DspFrameCounter = 0x7FFE;                  ///< Last u16 of each region
    static constexpr u32    DspSampleRate = 32728;
    static constexpr u32    SamplesPerTick = 160;
    static constexpr u32    AudioRingSize = 32768;                     ///< Stereo samples, about 1 s

    static u32             *g_audioRing = nullptr;      ///< Left | Right << 16
    static std::atomic<u32> g_audioHead{0};             ///< Written by the audio thread
    static std::atomic<u32> g_audioTail{0};             ///< Written by the worker saving the file
    static std::atomic<u32> g_audioLost{0};             ///< Samples dropped because the ring was full
    static std::atomic<bool> g_audioStarted{false};     ///< First tick captured
    static u64              g_audioStartTick;           ///< When the first tick was captured
    static bool             g_audioEnabled = false;     ///< Set by Start()
    static ThreadEx        *g_audioThread = nullptr;

    // Process::CheckAddress() only knows the game's regular memory and skips
    // IO mappings like the DSP's, so ask the kernel directly
    static bool     IsReadable(u32 address)
    {
        MemInfo info;
        PageInfo page;

        if (R_FAILED(svcQueryMemory(&info, &page, address)))
            return false;
        return info.state != MEMSTATE_FREE && (info.perm & MEMPERM_READ);
    }

    // Same rule the DSP uses to pick a region, including counter wraparound
    static u32      NewerRegion(u16 c0, u16 c1)
    {
        if (c0 == 0xFFFF && c1 != 0xFFFE)
            return 1;
        if (c1 == 0xFFFF && c0 != 0xFFFE)
            return 0;
        return c0 > c1 ? 0 : 1;
    }

    // `samples` null: silence
    static void     PushAudio(const u32 *samples, u32 count)
    {
        u32 head = g_audioHead;

        if (AudioRingSize - (head - g_audioTail) < count)
        {
            // The workers are behind. Remember the gap so the audio stays in sync.
            g_audioLost += count;
            g_stats.audioLostSamples += count;
            return;
        }

        for (u32 i = 0; i < count; ++i)
            g_audioRing[(head + i) % AudioRingSize] = samples ? samples[i] : 0;
        g_audioHead = head + count;
    }

    static void     AudioMain(void *)
    {
        bool tracking = false;
        u16 last = 0;
        u32 tick[SamplesPerTick];

        while (true)
        {
            svcSleepThread(2000000);

            if (g_state != Recording || !g_audioEnabled)
            {
                tracking = false;
                continue;
            }

            const u16 c0 = *reinterpret_cast<vu16 *>(DspRegions[0] + DspFrameCounter);
            const u16 c1 = *reinterpret_cast<vu16 *>(DspRegions[1] + DspFrameCounter);
            const u32 region = NewerRegion(c0, c1);
            const u16 counter = region ? c1 : c0;

            if (!tracking)
            {
                // Start with the next tick
                tracking = true;
                last = counter;
                continue;
            }

            u16 delta = counter - last;

            if (!delta)
                continue;

            // libctru skips counter 0 when it wraps around
            if (counter < last && delta <= 3)
                delta = 1;
            last = counter;

            // Ticks that went by while this thread didn't run: fill with
            // silence to keep the timing
            if (delta > 1)
            {
                g_stats.audioMissedTicks += delta - 1;
                PushAudio(nullptr, (delta - 1 < 40 ? delta - 1 : 40) * SamplesPerTick);
            }

            const vu32 *src = reinterpret_cast<const vu32 *>(DspRegions[region] + DspFinalSamples);
            for (u32 i = 0; i < SamplesPerTick; ++i)
                tick[i] = src[i];

            if (!g_audioStarted)
            {
                g_audioStartTick = svcGetSystemTick();
                g_audioStarted = true;
            }
            PushAudio(tick, SamplesPerTick);
            ++g_stats.audioTicks;
        }
    }

    //--------------------------------------------------------------------------
    // Encoding and writing (worker threads)
    //
    // Up to two workers run at the lowest priority, one per CPU core, so they
    // only use time the game leaves free. Each one takes the next captured
    // frame, compresses it, and then whoever holds the write lock saves the
    // finished frames to the SD card in order.
    //--------------------------------------------------------------------------

    class SdSink : public AviSink
    {
    public:
        File    file;

        bool    Write(const void *data, u32 size) override
        {
            return file.Write(data, size) == File::SUCCESS;
        }

        bool    Seek(u32 offset) override
        {
            return file.Seek(offset, File::SET) == File::SUCCESS;
        }
    };

    struct Session
    {
        SdSink          sink;
        AviWriter       avi;
        std::string     baseName;
        u32             part{0};
        u32             written{0};     ///< Next frame number expected in the video
        u32             firstFrame{0};
        bool            open{false};
        bool            started{false}; ///< First real frame seen
    };

    static constexpr u32    MaxWorkers = 2;

    static ThreadEx        *g_workers[MaxWorkers];
    static u32              g_workerCount = 0;
    static std::atomic<u32> g_claim{0};             ///< Next frame to encode
    static std::atomic<u32> g_encoded[SlotCount];   ///< 1 once a slot is ready to be written
    static u8              *g_out[SlotCount];       ///< Compressed frames (MJPEG only)
    static u32              g_outSize[SlotCount];
    static u32              g_outCapacity = 0;
    static LightLock        g_writeLock;            ///< Guards g_session and the SD writes
    static Session         *g_session = nullptr;
    static Codec            g_codec;
    static int              g_quality;

    //--------------------------------------------------------------------------
    // Audio output: ring buffer -> IMA ADPCM blocks in the AVI. Only touched
    // while holding g_writeLock.
    //--------------------------------------------------------------------------

    static constexpr u32    AudioBlocksPerChunk = 4;    ///< About 0.12 s of audio per write
    static constexpr u32    AudioSamplesPerBlock = ImaAdpcmEncoder::SamplesPerBlock;

    static AudioFormat      g_audioFormat;
    static ImaAdpcmEncoder  g_adpcm;
    static bool             g_audioMono;            ///< "Small": mono at half the sample rate
    static s16              g_audioPcm[AudioSamplesPerBlock * 2];
    static u32              g_audioPcmCount;        ///< Samples per channel in g_audioPcm
    static s32              g_audioDecimate[3];     ///< Mono: sum of L + R and sample count
    static u8               g_audioChunk[AviWriter::ChunkHeaderSize
                                         + AudioBlocksPerChunk * ImaAdpcmEncoder::BlockBytesPerChannel * 2 + 1];
    static u32              g_audioBlocks;          ///< Encoded blocks waiting in g_audioChunk
    static bool             g_audioSynced;
    static u32              g_audioSkip;            ///< Captured samples from before the video started

    static void     ResetAudioOutput(u32 mode)
    {
        g_audioMono = mode == 2;
        g_adpcm.Reset(g_audioMono ? 1 : 2);
        g_audioFormat.sampleRate = g_audioMono ? DspSampleRate / 2 : DspSampleRate;
        g_audioFormat.channels = g_adpcm.Channels();
        g_audioFormat.blockAlign = g_adpcm.BlockSize();
        g_audioFormat.samplesPerBlock = AudioSamplesPerBlock;
        g_audioPcmCount = 0;
        g_audioDecimate[0] = g_audioDecimate[1] = g_audioDecimate[2] = 0;
        g_audioBlocks = 0;
        g_audioSynced = false;
        g_audioSkip = 0;
    }

    static bool     FlushAudioChunk(Session &s)
    {
        bool ok = s.avi.WriteAudio(g_audioChunk, g_audioBlocks);

        g_audioBlocks = 0;
        return ok;
    }

    static bool     AddAudioSample(Session &s, s32 left, s32 right)
    {
        if (g_audioMono)
        {
            // Average 2 stereo samples into 1 mono sample (simple low-pass +
            // half the sample rate)
            g_audioDecimate[0] += left + right;
            if (++g_audioDecimate[2] < 2)
                return true;

            g_audioPcm[g_audioPcmCount] = g_audioDecimate[0] / 4;
            g_audioDecimate[0] = g_audioDecimate[2] = 0;
        }
        else
        {
            g_audioPcm[g_audioPcmCount * 2] = left;
            g_audioPcm[g_audioPcmCount * 2 + 1] = right;
        }

        if (++g_audioPcmCount < AudioSamplesPerBlock)
            return true;

        g_adpcm.EncodeBlock(g_audioPcm, g_audioChunk + AviWriter::ChunkHeaderSize + g_audioBlocks * g_adpcm.BlockSize());
        g_audioPcmCount = 0;

        if (++g_audioBlocks == AudioBlocksPerChunk)
            return FlushAudioChunk(s);
        return true;
    }

    // Moves captured audio into the file. `final`: the recording is ending,
    // write everything including a last, silence-padded block.
    static bool     SaveAudio(Session &s, bool final)
    {
        if (!g_audioEnabled || !s.started)
            return true;

        if (!g_audioSynced)
        {
            if (!g_audioStarted)
                return true;

            // Line the audio up with the first video frame: drop samples from
            // before it, or start with silence if the audio started later.
            u64 videoStart = g_startTick + (u64)s.firstFrame * SYSCLOCK_ARM11 / g_fps;
            s64 offset = (s64)(videoStart - g_audioStartTick) * DspSampleRate / (s64)SYSCLOCK_ARM11;

            if (offset > 0)
                g_audioSkip = (u32)offset;
            else
            {
                for (s64 i = offset < -(s64)DspSampleRate ? -(s64)DspSampleRate : offset; i < 0; ++i)
                {
                    if (!AddAudioSample(s, 0, 0))
                        return false;
                }
            }
            g_audioSynced = true;
        }

        u32 tail = g_audioTail;
        const u32 head = g_audioHead;

        for (; tail != head; ++tail)
        {
            if (g_audioSkip)
            {
                --g_audioSkip;
                continue;
            }

            u32 sample = g_audioRing[tail % AudioRingSize];
            if (!AddAudioSample(s, (s16)(sample & 0xFFFF), (s16)(sample >> 16)))
            {
                g_audioTail = tail;
                return false;
            }
        }
        g_audioTail = tail;

        for (u32 lost = g_audioLost.exchange(0); lost; --lost)
        {
            if (!AddAudioSample(s, 0, 0))
                return false;
        }

        if (!final)
            return true;

        while (g_audioPcmCount)
        {
            if (!AddAudioSample(s, 0, 0))
                return false;
        }
        return FlushAudioChunk(s);
    }

    static std::string  MakeBaseName(void)
    {
        time_t now = time(nullptr);
        struct tm *t = gmtime(&now); // The 3DS clock already is local time

        if (!t)
            return Utils::Format("%s/BinguRec_%08X", OutputDir, (u32)svcGetSystemTick());

        return Utils::Format("%s/BinguRec_%04d-%02d-%02d_%02d-%02d-%02d", OutputDir,
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
    }

    static std::string  PartPath(const Session &s)
    {
        if (s.part == 0)
            return s.baseName + ".avi";
        return Utils::Format("%s_part%u.avi", s.baseName.c_str(), s.part + 1);
    }

    static bool     OpenPart(Session &s)
    {
        std::string path = PartPath(s);

        if (File::Open(s.sink.file, path, File::RWC | File::TRUNCATE) != File::SUCCESS)
            return false;

        s.open = true;
        VideoFormat video = { g_width, g_height, g_fps, g_codec, g_outCapacity };
        return s.avi.Begin(&s.sink, video, g_audioEnabled ? &g_audioFormat : nullptr);
    }

    static bool     ClosePart(Session &s)
    {
        if (!s.open)
            return true;

        bool ok = s.avi.Finish();

        s.sink.file.Close();
        s.open = false;
        return ok;
    }

    // Saves one frame (size 0: couldn't be encoded), preceded by repeats for
    // any skipped frames
    static bool     SaveFrame(Session &s, u8 *chunk, u32 size, u32 frameNo)
    {
        if (!size)
            return true; // shows up as a repeat of the previous frame

        if (!s.started)
        {
            // Time starts at the first captured frame
            s.started = true;
            s.firstFrame = frameNo;
            s.written = frameNo;
        }

        u32 gap = frameNo > s.written ? frameNo - s.written : 0;

        if (s.avi.ProjectedSize(gap + 1, size) > MaxFileSize)
        {
            if (!ClosePart(s))
                return false;
            ++s.part;
            if (!OpenPart(s))
                return false;
        }

        if (gap && !s.avi.WriteDropped(gap))
            return false;
        if (!s.avi.WriteFrame(chunk, size))
            return false;

        s.written = frameNo + 1;
        return true;
    }

    // Compresses the next captured frame, if there is one nobody took yet
    static bool     EncodeNext(JpegEncoder &encoder, int &quality, u32 worker)
    {
        u32 frame = g_claim;

        do
        {
            if (frame == g_head)
                return false;
        } while (!g_claim.compare_exchange_weak(frame, frame + 1));

        u32 slot = frame % SlotCount;

        if (g_codec == Codec::Mjpeg)
        {
            if (quality != g_quality)
                encoder.SetQuality(quality = g_quality);

            const u16 *pixels = reinterpret_cast<const u16 *>(g_slots[slot] + AviWriter::ChunkHeaderSize);

            u64 start = svcGetSystemTick();

            // -1: keep a spare byte for the AVI padding
            g_outSize[slot] = encoder.Encode(pixels, g_width, g_height,
                g_out[slot] + AviWriter::ChunkHeaderSize, g_outCapacity - AviWriter::ChunkHeaderSize - 1);

            g_stats.encodeTicks[worker] += svcGetSystemTick() - start;
            ++g_stats.encoded[worker];
            if (!g_outSize[slot])
                ++g_stats.tooBig;
        }
        else
            g_outSize[slot] = g_width * g_height * 2;

        g_encoded[slot] = 1;
        return true;
    }

    // Saves finished frames in order. Only one thread writes at a time; if
    // another one is busy writing it will pick these frames up too.
    static void     WriteReady(void)
    {
        if (LightLock_TryLock(&g_writeLock))
            return;

        while (g_session && g_tail != g_head)
        {
            u32 slot = g_tail % SlotCount;

            if (!g_encoded[slot])
                break;

            u8 *chunk = g_codec == Codec::Mjpeg ? g_out[slot] : g_slots[slot];

            if (!g_failed && !SaveFrame(*g_session, chunk, g_outSize[slot], g_slotFrame[slot]))
                g_failed = true;

            g_encoded[slot] = 0;
            ++g_tail;
        }

        if (g_session && !g_failed && !SaveAudio(*g_session, false))
            g_failed = true;

        LightLock_Unlock(&g_writeLock);
    }

    static void     WriteReport(const Session &s, u32 seconds);

    // Opens and finishes the files. Done by the first worker only.
    static void     ManageSession(void)
    {
        u32 state = g_state;

        LightLock_Lock(&g_writeLock);

        if (state != Idle && !g_session)
        {
            g_session = new Session;
            g_session->baseName = MakeBaseName();

            if (!Directory::IsExists(OutputDir))
                Directory::Create(OutputDir);

            if (!OpenPart(*g_session))
                g_failed = true;
        }

        if (g_failed && state == Recording)
        {
            // Stop capturing, the SD card is full or unavailable
            LightLock_Lock(&g_lock);
            g_stopFrame = 0;
            g_state = Stopping;
            LightLock_Unlock(&g_lock);
        }

        if (state != Stopping || g_tail != g_head)
        {
            LightLock_Unlock(&g_writeLock);
            return;
        }

        // Recording stopped and every frame is saved: finish the file
        Session *session = g_session;
        bool ok = !g_failed;
        u32 seconds = 0;

        if (ok && session->started)
        {
            // Pad up to the moment the user pressed stop
            if (g_stopFrame > session->written)
                ok = session->avi.WriteDropped(g_stopFrame - session->written);
            ok = ok && SaveAudio(*session, true);
            seconds = ((g_stopFrame > session->written ? g_stopFrame : session->written) - session->firstFrame) / g_fps;
        }

        if (!ClosePart(*session))
            ok = false;

        if (ok && !session->started)
            File::Remove(PartPath(*session)); // nothing was captured

        if (!ok)
            OSD::Notify("BinguRecorder: could not save, is the SD card full?", Color::White, Color::Red);
        else if (!session->started)
            OSD::Notify("BinguRecorder: nothing was recorded");
        else
        {
            std::string name = PartPath(*session);
            name = name.substr(name.rfind('/') + 1);
            bool noSound = Recorder::settings.audio && !(g_audioEnabled && g_audioStarted);
            OSD::Notify(Utils::Format("Saved %s (%u:%02u)%s", name.c_str(), seconds / 60, seconds % 60,
                noSound ? " - no sound" : ""), Color::White, Color::ForestGreen);
        }

        if (session->started)
            WriteReport(*session, seconds);

        delete session;
        g_session = nullptr;
        g_failed = false;
        g_state = Idle;
        LightLock_Unlock(&g_writeLock);
        LightEvent_Signal(&g_idle);
    }

    static void     WriteReport(const Session &s, u32 seconds)
    {
        static const char *screens[] = { "Top", "Bottom", "Both" };
        const Settings &set = Recorder::settings;
        const float secs = seconds ? (float)seconds : 1.f;
        File file;

        if (File::Open(file, s.baseName + ".txt", File::RWC | File::TRUNCATE) != File::SUCCESS)
            return;

        auto avgMs = [](u32 w) -> float
        {
            return g_stats.encoded[w] ? (float)g_stats.encodeTicks[w] * 1000.f / SYSCLOCK_ARM11 / g_stats.encoded[w] : 0.f;
        };

        file.WriteLine("BinguRecorder report");
        file.WriteLine(Utils::Format("Settings: screen %s, %s size, %u fps, compression %s, quality %u, sound %s, priority %s",
            screens[set.screens], set.halfSize ? "half" : "full", set.fps, set.compress ? "on" : "off", set.quality,
            set.audio == 0 ? "off" : set.audio == 1 ? "on" : "small", set.priority ? "recording" : "game"));
        file.WriteLine(Utils::Format("Length: %u s, video %ux%u", seconds, g_width, g_height));
        file.WriteLine(Utils::Format("Game frames shown: %u (%.1f per second)", g_stats.gameFrames, g_stats.gameFrames / secs));
        file.WriteLine(Utils::Format("Frames captured: %u (%.1f per second), skipped because still saving: %u",
            g_stats.captured, g_stats.captured / secs, g_stats.skippedBusy));
        file.WriteLine(Utils::Format("Worker on game core: %u frames, %.1f ms each", g_stats.encoded[0], avgMs(0)));
        if (g_workerCount > 1)
            file.WriteLine(Utils::Format("Worker on system core: %u frames, %.1f ms each", g_stats.encoded[1], avgMs(1)));
        else
            file.WriteLine("Worker on system core: could not be started");
        file.WriteLine(Utils::Format("Frames too big to save: %u", g_stats.tooBig));
        file.WriteLine(Utils::Format("Sound: %s, %u ticks captured (%.0f per second, 204.5 expected), %u missed, %u samples lost",
            g_stats.audioStatus, g_stats.audioTicks, g_stats.audioTicks / secs, g_stats.audioMissedTicks, g_stats.audioLostSamples));
        file.Close();
    }

    static void     WorkerMain(void *arg)
    {
        const u32 worker = reinterpret_cast<u32>(arg);
        const bool manager = worker == 0;
        JpegEncoder encoder;
        int quality = -1;

        while (true)
        {
            // Producer signals new frames; the timeout keeps both workers
            // polling since a one-shot event only wakes one of them.
            LightEvent_WaitTimeout(&g_wake, g_state == Idle ? 100000000LL : 10000000LL);

            if (g_state == Idle)
                continue;

            while (EncodeNext(encoder, quality, worker))
                WriteReady();
            WriteReady();

            if (manager)
                ManageSession();
        }
    }

    // Allocates `count` buffers of `size` bytes, keeping the old ones if they
    // are already big enough. Nothing touches them while idle, so it's safe.
    static bool     Reserve(u8 **buffers, u32 &capacity, u32 size)
    {
        if (size <= capacity)
            return true;

        capacity = size;
        for (u32 i = 0; i < SlotCount; ++i)
        {
            ::operator delete(buffers[i]);
            buffers[i] = static_cast<u8 *>(::operator new(size, std::nothrow));
            if (!buffers[i])
                capacity = 0;
        }

        if (capacity)
            return true;

        for (u32 i = 0; i < SlotCount; ++i)
        {
            ::operator delete(buffers[i]);
            buffers[i] = nullptr;
        }
        return false;
    }

    // Worker 0 shares the game's core: by default it only uses time the game
    // leaves free (lowest priority), with "Priority: Recording" it goes ahead
    // of the game, but never level with the audio thread (equal priorities
    // don't preempt each other, and audio has to be read every ~5 ms). Worker 1 is on the system core, where the game's main
    // thread never runs, so it can always have a fairly high priority (still
    // below the game's sound threads, which run around 0x18-0x20 there).
    static s32      WorkerPriority(u32 worker, Codec codec, u8 priority)
    {
        if (worker == 1)
            return 0x28;
        if (priority)
            return 0x20; // ahead of a typical game (~0x30), behind audio capture (0x18)
        // Raw frames only need writing, which mostly waits on the SD card
        return codec == Codec::Mjpeg ? 0x3F : 0x2E;
    }

    static bool     StartWorkers(void)
    {
        if (g_workerCount)
            return true;

        // One worker on the game's core, and if the game allows it, a second
        // one on the system core.
        static const int affinities[MaxWorkers] = { -2, 1 };

        // Builds the encoder's shared tables before two threads can race on it
        JpegEncoder tables;
        (void)tables;

        for (u32 i = 0; i < MaxWorkers; ++i)
        {
            ThreadEx *thread = new ThreadEx(WorkerMain, 0x4000, 0x3F, affinities[i]);

            if (R_FAILED(thread->Start(reinterpret_cast<void *>(i))))
            {
                delete thread;
                break;
            }
            g_workers[g_workerCount++] = thread;
        }

        return g_workerCount > 0;
    }

    //--------------------------------------------------------------------------
    // Public API
    //--------------------------------------------------------------------------

    static void     OnPauseResume(bool goingToPause)
    {
        // Don't record the time spent in the plugin menu
        LightLock_Lock(&g_lock);
        g_paused = goingToPause;
        if (goingToPause)
            g_pauseTick = svcGetSystemTick();
        else if (g_pauseTick)
        {
            g_startTick += svcGetSystemTick() - g_pauseTick;
            g_pauseTick = 0;
        }
        LightLock_Unlock(&g_lock);
    }

    static void     OnProcessEvent(Process::Event event)
    {
        // Make sure the video is complete before the game goes away
        if (event == Process::Event::EXIT || event == Process::Event::HOME_ENTER
            || event == Process::Event::SLEEP_ENTER)
            Recorder::Stop(true);
    }

    void    Recorder::Initialize(void)
    {
        LightLock_Init(&g_lock);
        LightLock_Init(&g_writeLock);
        LightEvent_Init(&g_wake, RESET_ONESHOT);
        LightEvent_Init(&g_idle, RESET_STICKY);
        LightEvent_Signal(&g_idle);

        settings.Load();

        Process::OnPauseResume = OnPauseResume;
        Process::SetProcessEventCallback(OnProcessEvent);
        OSD::Run(OnScreenFrame);
    }

    void    Recorder::Start(void)
    {
        if (g_state != Idle)
            return;

        const CaptureScreens screens = (CaptureScreens)settings.screens;
        const u32 div = settings.halfSize ? 2 : 1;
        const u32 width = (screens == CaptureScreens::Bottom ? 320 : 400) / div;
        const u32 height = (screens == CaptureScreens::Both ? 480 : 240) / div;
        const u32 slotSize = AviWriter::ChunkHeaderSize + width * height * 2;
        const Codec codec = settings.compress ? Codec::Mjpeg : Codec::Raw;
        // JPEG frames stay far below 1 byte per pixel; one that doesn't fit
        // is simply skipped. +1 for the AVI padding byte.
        const u32 outSize = AviWriter::ChunkHeaderSize + width * height + 1;

        // Buffers are allocated on first use, so the plugin costs no memory
        // until then, and only grow when a bigger mode is picked.
        if (!Reserve(g_slots, g_slotSize, slotSize + 1)
            || (codec == Codec::Mjpeg && !Reserve(g_out, g_outCapacity, outSize)))
        {
            OSD::Notify("BinguRecorder: not enough memory, try Half size", Color::White, Color::Red);
            return;
        }

        // Sound needs the DSP's memory, which games that use sound have mapped
        bool audio = settings.audio != 0 && IsReadable(DspRegions[0]) && IsReadable(DspRegions[1]);

        if (audio && !g_audioRing)
            g_audioRing = static_cast<u32 *>(::operator new(AudioRingSize * sizeof(u32), std::nothrow));

        if (audio && !g_audioThread && g_audioRing)
        {
            // High priority but it only runs for a few microseconds every 2 ms
            g_audioThread = new ThreadEx(AudioMain, 0x1000, 0x18, -2);
            if (R_FAILED(g_audioThread->Start(nullptr)))
            {
                delete g_audioThread;
                g_audioThread = nullptr;
            }
        }
        audio = audio && g_audioThread;

        g_stats = Stats();
        g_stats.lastBusyFrame = ~0u;
        g_stats.audioStatus = !settings.audio ? "off"
            : audio ? "on"
            : !IsReadable(DspRegions[0]) ? "not available (sound memory not readable)"
            : "not available (could not start)";

        if (!StartWorkers())
        {
            OSD::Notify("BinguRecorder: could not start the recording threads", Color::White, Color::Red);
            return;
        }

        for (u32 i = 0; i < g_workerCount; ++i)
            svcSetThreadPriority(g_workers[i]->GetHandle(), WorkerPriority(i, codec, settings.priority));

        LightLock_Lock(&g_lock);

        g_screens = screens;
        g_half = div == 2;
        g_fps = settings.fps;
        g_width = width;
        g_height = height;
        g_codec = codec;
        g_quality = settings.quality;

        g_audioEnabled = audio;
        g_audioHead = 0;
        g_audioTail = 0;
        g_audioLost = 0;
        g_audioStarted = false;
        ResetAudioOutput(settings.audio);

        // Black background for the borders around the bottom screen
        for (u32 i = 0; i < SlotCount; ++i)
            std::memset(g_slots[i], 0, slotSize);

        g_bottom = ScreenInfo();
        g_head = 0;
        g_tail = 0;
        g_claim = 0;
        for (u32 i = 0; i < SlotCount; ++i)
            g_encoded[i] = 0;
        g_nextFrame = 0;
        g_stopFrame = 0;
        g_failed = false;
        g_startTick = svcGetSystemTick();
        // Started from the plugin menu: the clock starts once the game resumes
        g_pauseTick = g_paused ? g_startTick : 0;
        LightEvent_Clear(&g_idle);
        g_state = Recording;

        LightLock_Unlock(&g_lock);
        LightEvent_Signal(&g_wake);

        // Notifications are drawn on the top screen and would end up in the video
        if (g_screens == CaptureScreens::Bottom)
            OSD::Notify("BinguRecorder: recording", Color::White, Color::Red);
    }

    void    Recorder::Stop(bool wait)
    {
        LightLock_Lock(&g_lock);
        if (g_state == Recording)
        {
            u64 now = g_pauseTick ? g_pauseTick : svcGetSystemTick();
            g_stopFrame = ElapsedFrames(now);
            g_state = Stopping;
        }
        LightLock_Unlock(&g_lock);

        LightEvent_Signal(&g_wake);

        if (wait && g_state != Idle)
            LightEvent_WaitTimeout(&g_idle, 5000000000LL);
    }

    void    Recorder::Toggle(void)
    {
        if (g_state == Idle)
            Start();
        else
            Stop();
    }

    bool    Recorder::IsRecording(void)
    {
        return g_state != Idle;
    }
}
