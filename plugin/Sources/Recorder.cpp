#include "Recorder.hpp"
#include "AviWriter.hpp"

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

    // Frame buffers between the game thread and the SD writer thread
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
    static std::atomic<u32> g_tail{0};  ///< Frames saved (written by the writer thread)
    static std::atomic<u32> g_state{Idle};
    static std::atomic<bool> g_failed{false};

    static LightLock        g_lock;     ///< Guards the recording session against the capture hook
    static LightEvent       g_wake;     ///< Wakes the writer thread
    static LightEvent       g_idle;     ///< Signaled once a recording is fully saved
    static ThreadEx        *g_writer = nullptr;

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
            if (loaded.screens <= (u8)CaptureScreens::Both && loaded.fps >= 1 && loaded.fps <= 30)
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

        // Walk the source sequentially (framebuffer reads are the slow part)
        for (u32 x = 0; x < columns; ++x, src += stride * step, ++dst)
        {
            const u8 *in = src;
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

        if (!src || !Process::CheckAddress(screen.address))
            return;

        // The GPU writes framebuffers behind the CPU's back: drop stale cache lines
        svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, screen.address, screen.stride * screenWidth);

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
                if (frameNo >= g_nextFrame && g_head - g_tail < SlotCount)
                {
                    CaptureFrame(screen);
                    g_slotFrame[g_head % SlotCount] = frameNo;
                    g_nextFrame = frameNo + 1;
                    ++g_head;
                    LightEvent_Signal(&g_wake);
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
    // Writer thread
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
        return s.avi.Begin(&s.sink, g_width, g_height, g_fps);
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

    // Saves one captured frame, preceded by repeats for any skipped frames
    static bool     SaveFrame(Session &s, u8 *chunk, u32 frameNo)
    {
        if (!s.started)
        {
            // Time starts at the first captured frame
            s.started = true;
            s.firstFrame = frameNo;
            s.written = frameNo;
        }

        u32 gap = frameNo > s.written ? frameNo - s.written : 0;

        if (s.avi.ProjectedSize(gap + 1) > MaxFileSize)
        {
            if (!ClosePart(s))
                return false;
            ++s.part;
            if (!OpenPart(s))
                return false;
        }

        if (gap && !s.avi.WriteDropped(gap))
            return false;
        if (!s.avi.WriteFrame(chunk))
            return false;

        s.written = frameNo + 1;
        return true;
    }

    static void     WriterMain(void *)
    {
        Session *session = nullptr;

        while (true)
        {
            LightEvent_WaitTimeout(&g_wake, 100000000LL); // 100ms, just in case

            u32 state = g_state;

            if (state == Idle)
                continue;

            if (!session)
            {
                session = new Session;
                session->baseName = MakeBaseName();

                if (!Directory::IsExists(OutputDir))
                    Directory::Create(OutputDir);

                if (!OpenPart(*session))
                    g_failed = true;
            }

            // Save everything that has been captured so far
            while (g_tail != g_head)
            {
                u32 slot = g_tail % SlotCount;

                if (!g_failed && !SaveFrame(*session, g_slots[slot], g_slotFrame[slot]))
                    g_failed = true;
                ++g_tail;
            }

            if (g_failed && state == Recording)
            {
                // Stop capturing, the SD card is full or unavailable
                LightLock_Lock(&g_lock);
                g_stopFrame = 0;
                g_state = Stopping;
                LightLock_Unlock(&g_lock);
                continue;
            }

            if (state != Stopping || g_tail != g_head)
                continue;

            // Recording stopped and every frame is saved: finish the file
            bool ok = !g_failed;
            u32 seconds = 0;

            if (ok && session->started)
            {
                // Pad up to the moment the user pressed stop
                if (g_stopFrame > session->written)
                    ok = session->avi.WriteDropped(g_stopFrame - session->written);
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
                OSD::Notify(Utils::Format("Saved %s (%u:%02u)", name.c_str(), seconds / 60, seconds % 60), Color::White, Color::ForestGreen);
            }

            delete session;
            session = nullptr;
            g_failed = false;
            g_state = Idle;
            LightEvent_Signal(&g_idle);
        }
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

        // Buffers are allocated on first use (so the plugin costs no memory
        // until then) and only grow when a bigger mode is picked. Nothing
        // touches them while idle, so swapping them here is safe.
        if (slotSize > g_slotSize)
        {
            for (u32 i = 0; i < SlotCount; ++i)
            {
                ::operator delete(g_slots[i]);
                g_slots[i] = static_cast<u8 *>(::operator new(slotSize, std::nothrow));
            }

            g_slotSize = slotSize;
            for (u32 i = 0; i < SlotCount; ++i)
            {
                if (!g_slots[i])
                    g_slotSize = 0;
            }

            if (!g_slotSize)
            {
                for (u32 i = 0; i < SlotCount; ++i)
                {
                    ::operator delete(g_slots[i]);
                    g_slots[i] = nullptr;
                }
                OSD::Notify("BinguRecorder: not enough memory, try Half size", Color::White, Color::Red);
                return;
            }
        }

        if (!g_writer)
        {
            g_writer = new ThreadEx(WriterMain, 0x4000, 0x2E, -2);
            if (R_FAILED(g_writer->Start(nullptr)))
            {
                delete g_writer;
                g_writer = nullptr;
                OSD::Notify("BinguRecorder: could not start writer thread", Color::White, Color::Red);
                return;
            }
        }

        LightLock_Lock(&g_lock);

        g_screens = screens;
        g_half = div == 2;
        g_fps = settings.fps;
        g_width = width;
        g_height = height;

        // Black background for the borders around the bottom screen
        for (u32 i = 0; i < SlotCount; ++i)
            std::memset(g_slots[i], 0, slotSize);

        g_bottom = ScreenInfo();
        g_head = 0;
        g_tail = 0;
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
