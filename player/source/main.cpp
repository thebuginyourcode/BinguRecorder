// BinguPlayer: plays BinguRecorder videos (sd:/BinguRecorder/*.avi) on the 3DS.
#include <3ds.h>
#include <turbojpeg.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "AviReader.hpp"
#include "FrameBlit.hpp"
#include "ImaAdpcmDecoder.hpp"

using namespace Bingu;

static const char  *VideoDir = "sdmc:/BinguRecorder";

struct VideoFile
{
    std::string name;
    u32         size;
};

static bool     g_soundAvailable = false;

//------------------------------------------------------------------------------
// File list
//------------------------------------------------------------------------------

static std::vector<VideoFile>   ListVideos(void)
{
    std::vector<VideoFile> files;
    DIR *dir = opendir(VideoDir);

    if (!dir)
        return files;

    while (struct dirent *entry = readdir(dir))
    {
        std::string name = entry->d_name;

        if (name.size() < 5)
            continue;

        std::string ext = name.substr(name.size() - 4);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".avi")
            continue;

        struct stat st;
        u32 size = stat((std::string(VideoDir) + "/" + name).c_str(), &st) == 0 ? (u32)st.st_size : 0;
        files.push_back({name, size});
    }
    closedir(dir);

    // Names contain the date and time: newest first
    std::sort(files.begin(), files.end(), [](const VideoFile &a, const VideoFile &b) { return a.name > b.name; });
    return files;
}

static std::string  FormatTime(double seconds)
{
    u32 s = seconds > 0 ? (u32)seconds : 0;
    char buf[32];

    if (s >= 3600)
        snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
    else
        snprintf(buf, sizeof(buf), "%lu:%02lu", s / 60, s % 60);
    return buf;
}

static void     DrawList(const std::vector<VideoFile> &files, u32 selected, const std::string &message)
{
    consoleClear();
    printf("\x1b[1;1H\x1b[36mBinguPlayer\x1b[0m\n");

    if (files.empty())
    {
        printf("\nNo videos found.\n\nRecord one with the BinguRecorder\nplugin, they are saved in\nsd:/BinguRecorder/\n");
    }
    else
    {
        const u32 visible = 20;
        u32 first = selected >= visible ? selected - visible + 1 : 0;

        for (u32 i = first; i < files.size() && i < first + visible; ++i)
        {
            const VideoFile &f = files[i];
            // "BinguRec_2026-10-24_13-46-34.avi" -> "2026-10-24 13:46:34"
            std::string label = f.name;
            if (label.compare(0, 9, "BinguRec_") == 0 && label.size() >= 32)
            {
                label = label.substr(9, 10) + " " + label.substr(20, 2) + ":" + label.substr(23, 2) + ":"
                    + label.substr(26, 2) + label.substr(28, label.size() - 32);
            }

            printf("\x1b[%lu;1H%s %-30.30s %5.1f MB\x1b[0m", 3 + (i - first),
                i == selected ? "\x1b[47;30m>" : " ", label.c_str(), f.size / 1048576.0);
        }
    }

    if (!message.empty())
        printf("\x1b[25;1H\x1b[31m%.40s\x1b[0m", message.c_str());
    printf("\x1b[27;1HA: play   START: exit");
    if (!g_soundAvailable)
        printf("\x1b[29;1H\x1b[33mNo sound: run DSP1 once to dump the\nsound firmware (sd:/3ds/dspfirm.cdc)\x1b[0m");
}

//------------------------------------------------------------------------------
// Playback
//------------------------------------------------------------------------------

class Player
{
public:
    std::string     Play(const std::string &path, const std::string &name);

private:
    static constexpr u32    WaveBufCount = 8;

    bool        DecodeFrame(u32 index);
    void        ShowFrame(void);
    void        QueueAudio(void);
    void        StartAt(double seconds);
    double      Now(void) const;
    void        DrawInfo(const std::string &name, double t, bool paused);

    AviReader               _avi;
    Layout                  _layout;
    tjhandle                _jpeg{nullptr};
    std::vector<u8>         _chunk;
    std::vector<u8>         _bgr;

    bool                    _audio{false};
    ndspWaveBuf             _wave[WaveBufCount];
    s16                    *_waveData[WaveBufCount]{};
    u32                     _waveCapacity{0};   ///< Samples per channel per buffer
    std::vector<u8>         _audioChunk;
    u32                     _nextAudio{0};      ///< Next audio chunk to queue

    u64                     _startTick{0};
    double                  _startTime{0};      ///< Video time at _startTick
    u64                     _pauseTick{0};
    int32_t                 _shown{-2};         ///< Frame currently on screen
};

double  Player::Now(void) const
{
    u64 now = _pauseTick ? _pauseTick : svcGetSystemTick();
    return _startTime + (double)(now - _startTick) / SYSCLOCK_ARM11;
}

bool    Player::DecodeFrame(u32 index)
{
    const AviReader::Chunk &c = _avi.Frame(index);

    if (!_avi.Read(c, _chunk.data()))
        return false;

    if (_avi.Codec() == AviReader::VideoCodec::Rgb555)
    {
        Rgb555ToBgr24(reinterpret_cast<const uint16_t *>(_chunk.data()), _avi.Width(), _avi.Height(), _bgr.data());
        return true;
    }

    return tjDecompress2(_jpeg, _chunk.data(), c.size, _bgr.data(), _avi.Width(), _avi.Width() * 3,
        _avi.Height(), TJPF_BGR, TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE) == 0;
}

void    Player::ShowFrame(void)
{
    if (_layout.top.visible)
    {
        FbTarget top = { gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr), 400, FbFormat::Bgr8 };
        Blit(_bgr.data(), _avi.Width(), _layout.top, _layout.scale, top);
    }
    if (_layout.bottom.visible)
    {
        // The bottom screen is the text console's (RGB565, single buffered)
        FbTarget bottom = { gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr), 320, FbFormat::Rgb565 };
        Blit(_bgr.data(), _avi.Width(), _layout.bottom, _layout.scale, bottom);
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
}

void    Player::QueueAudio(void)
{
    if (!_audio)
        return;

    for (ndspWaveBuf &wb : _wave)
    {
        if (wb.status != NDSP_WBUF_FREE && wb.status != NDSP_WBUF_DONE)
            continue;
        if (_nextAudio >= _avi.AudioChunkCount())
            return;

        const AviReader::AudioChunk &c = _avi.Audio(_nextAudio++);
        if (!_avi.Read(c, _audioChunk.data()))
            continue;

        const u32 blocks = std::min(c.size / _avi.BlockAlign(), _waveCapacity / _avi.SamplesPerBlock());
        s16 *out = const_cast<s16 *>(wb.data_pcm16);

        for (u32 b = 0; b < blocks; ++b)
        {
            DecodeImaAdpcmBlock(_audioChunk.data() + b * _avi.BlockAlign(), _avi.BlockAlign(), _avi.Channels(),
                _avi.SamplesPerBlock(), out + b * _avi.SamplesPerBlock() * _avi.Channels());
        }

        wb.nsamples = blocks * _avi.SamplesPerBlock();
        DSP_FlushDataCache(out, wb.nsamples * _avi.Channels() * sizeof(s16));
        ndspChnWaveBufAdd(0, &wb);
    }
}

// Jumps to `seconds` (snapped to the start of an audio chunk so sound and
// picture stay in sync)
void    Player::StartAt(double seconds)
{
    seconds = std::max(0.0, std::min(seconds, _avi.Duration()));

    if (_audio)
    {
        ndspChnWaveBufClear(0);
        for (ndspWaveBuf &wb : _wave)
            wb.status = NDSP_WBUF_FREE;

        _nextAudio = _avi.AudioChunkAt((u32)(seconds * _avi.SampleRate()));
        if (_nextAudio < _avi.AudioChunkCount())
            seconds = (double)_avi.Audio(_nextAudio).firstSample / _avi.SampleRate();
        QueueAudio();
    }

    _startTime = seconds;
    _startTick = svcGetSystemTick();
    if (_pauseTick)
        _pauseTick = _startTick;
    _shown = -2;
}

void    Player::DrawInfo(const std::string &name, double t, bool paused)
{
    if (_layout.bottom.visible)
        return; // the bottom screen shows the video

    const double total = _avi.Duration();
    const u32 bar = total > 0 ? (u32)(38 * std::min(1.0, t / total)) : 0;
    std::string progress(bar, '=');
    progress.resize(38, '-');

    printf("\x1b[1;1H\x1b[36mBinguPlayer\x1b[0m\n\n%-40.40s\n\n", name.c_str());
    printf("%s / %s   %s        \n\n", FormatTime(t).c_str(), FormatTime(total).c_str(), paused ? "\x1b[33mPaused\x1b[0m" : "      ");
    printf("[%s]\n\n", progress.c_str());
    printf("%lux%lu, %.0f fps%s\n", _avi.Width(), _avi.Height(), _avi.Fps(),
        _avi.HasAudio() ? (_audio ? ", sound" : ", sound (needs dspfirm.cdc)") : ", no sound");
    printf("\x1b[24;1HA: pause/play\nLeft/Right: -/+ 10 seconds\nB: back to the list");
}

std::string     Player::Play(const std::string &path, const std::string &name)
{
    std::string error = _avi.Open(path);
    if (!error.empty())
        return error;

    _layout = ChooseLayout(_avi.Width(), _avi.Height());
    _chunk.resize(std::max(_avi.MaxVideoChunk(), _avi.Width() * _avi.Height() * 2));
    _bgr.assign(_avi.Width() * _avi.Height() * 3, 0);

    if (_avi.Codec() == AviReader::VideoCodec::Mjpeg && !(_jpeg = tjInitDecompress()))
        return "Could not start the JPEG decoder";

    // Sound
    _audio = g_soundAvailable && _avi.HasAudio() && _avi.AudioChunkCount();
    if (_audio)
    {
        u32 maxBlocks = _avi.MaxAudioChunk() / _avi.BlockAlign();
        _waveCapacity = maxBlocks * _avi.SamplesPerBlock();
        _audioChunk.resize(_avi.MaxAudioChunk());

        for (u32 i = 0; i < WaveBufCount && _audio; ++i)
        {
            _waveData[i] = static_cast<s16 *>(linearAlloc(_waveCapacity * _avi.Channels() * sizeof(s16)));
            _audio = _waveData[i] != nullptr;
            std::memset(&_wave[i], 0, sizeof(ndspWaveBuf));
            _wave[i].data_pcm16 = _waveData[i];
            _wave[i].status = NDSP_WBUF_FREE;
        }

        if (_audio)
        {
            float mix[12] = { 1.0f, 1.0f };
            ndspChnReset(0);
            ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
            ndspChnSetRate(0, (float)_avi.SampleRate());
            ndspChnSetFormat(0, _avi.Channels() == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
            ndspChnSetMix(0, mix);
        }
    }

    // Black background on every buffer of the screens we draw on
    consoleClear();
    for (int i = 0; i < 2; ++i)
    {
        FbTarget top = { gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr), 400, FbFormat::Bgr8 };
        Clear(top);
        if (_layout.bottom.visible)
        {
            FbTarget bottom = { gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr), 320, FbFormat::Rgb565 };
            Clear(bottom);
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
    }

    StartAt(0);

    u32 infoTimer = 0;
    bool finished = false;

    while (aptMainLoop() && !finished)
    {
        hidScanInput();
        u32 down = hidKeysDown();

        if (down & KEY_B)
            break;

        if (down & KEY_A)
        {
            if (_pauseTick)
            {
                _startTick += svcGetSystemTick() - _pauseTick;
                _pauseTick = 0;
            }
            else
                _pauseTick = svcGetSystemTick();
            if (_audio)
                ndspChnSetPaused(0, _pauseTick != 0);
        }

        if (down & (KEY_LEFT | KEY_RIGHT))
            StartAt(Now() + (down & KEY_RIGHT ? 10.0 : -10.0));

        QueueAudio();

        // Show the frame that belongs to the current time. Frames we are too
        // slow to decode are skipped automatically.
        const double t = Now();
        const u32 frame = (u32)(t * _avi.Fps());

        if (frame >= _avi.FrameCount())
        {
            // Done once the picture and the queued sound have finished
            bool soundDone = !_audio || _nextAudio >= _avi.AudioChunkCount();
            for (ndspWaveBuf &wb : _wave)
                soundDone = soundDone && (!_audio || (wb.status != NDSP_WBUF_QUEUED && wb.status != NDSP_WBUF_PLAYING));
            finished = soundDone && !_pauseTick;
        }
        else
        {
            int32_t real = _avi.LastRealFrame(frame);

            if (real >= 0 && real != _shown && DecodeFrame(real))
            {
                ShowFrame();
                _shown = real;
            }
        }

        if (++infoTimer % 15 == 1)
            DrawInfo(name, std::min(t, _avi.Duration()), _pauseTick != 0);

        gspWaitForVBlank();
    }

    // Clean up
    if (_audio)
    {
        ndspChnWaveBufClear(0);
        ndspChnReset(0);
    }
    for (s16 *&data : _waveData)
    {
        if (data)
            linearFree(data);
        data = nullptr;
    }
    if (_jpeg)
        tjDestroy(_jpeg);
    _jpeg = nullptr;
    _avi.Close();

    for (int i = 0; i < 2; ++i)
    {
        FbTarget top = { gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr), 400, FbFormat::Bgr8 };
        Clear(top);
        gfxFlushBuffers();
        gfxSwapBuffers();
    }
    return "";
}

int     main(void)
{
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, nullptr);
    osSetSpeedupEnable(true); // New 3DS: faster CPU

    // Needs the DSP firmware dumped by DSP1 (sd:/3ds/dspfirm.cdc)
    g_soundAvailable = R_SUCCEEDED(ndspInit());
    if (g_soundAvailable)
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    std::vector<VideoFile> files = ListVideos();
    u32 selected = 0;
    std::string message;
    bool redraw = true;

    while (aptMainLoop())
    {
        if (redraw)
        {
            DrawList(files, selected, message);
            redraw = false;
        }

        hidScanInput();
        u32 down = hidKeysDown();

        if (down & KEY_START)
            break;

        if (!files.empty() && (down & (KEY_DOWN | KEY_UP)))
        {
            selected = (selected + (down & KEY_DOWN ? 1 : files.size() - 1)) % files.size();
            message.clear();
            redraw = true;
        }

        if (!files.empty() && (down & KEY_A))
        {
            const VideoFile &f = files[selected];
            Player *player = new Player;

            message = player->Play(std::string(VideoDir) + "/" + f.name, f.name);
            delete player;
            files = ListVideos();
            selected = std::min<u32>(selected, files.empty() ? 0 : files.size() - 1);
            redraw = true;
        }

        gspWaitForVBlank();
    }

    if (g_soundAvailable)
        ndspExit();
    gfxExit();
    return 0;
}
