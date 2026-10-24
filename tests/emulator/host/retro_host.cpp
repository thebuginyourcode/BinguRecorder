// Minimal headless libretro frontend for testing BinguRecorder in an
// emulator (Azahar's libretro core with the software renderer).
//
// usage: retro_host <core.so> <content> <data dir> <frames> [options]
//   --press FROM:TO:BUTTONS   hold buttons (e.g. l+r+down) during frames FROM..TO
//   --shot FRAME              save the screen at FRAME as data dir/shot_FRAME.ppm
//   --audio FILE              write the emulator's sound output (s16le stereo)
//
// The emulated SD card is <data dir>/Azahar/sdmc.
#include "libretro.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <set>
#include <string>
#include <vector>

struct Press
{
    unsigned    from, to;
    unsigned    buttons;    // bit per RETRO_DEVICE_ID_JOYPAD_*
};

static std::string              g_dataDir;
static std::vector<Press>       g_presses;
static std::set<unsigned>       g_shots;
static unsigned                 g_frame = 0;
static retro_pixel_format       g_pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
static FILE                    *g_audio = nullptr;

static void     Log(enum retro_log_level level, const char *fmt, ...)
{
    if (level < RETRO_LOG_INFO)
        return;
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

static bool     Environment(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = g_dataDir.c_str();
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        g_pixelFormat = *(const retro_pixel_format *)data;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((retro_log_callback *)data)->log = Log;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;
    default:
        // Variables fall back to the core's defaults; no hardware rendering
        return false;
    }
}

static void     SavePpm(const void *data, unsigned width, unsigned height, size_t pitch, const std::string &path)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return;

    fprintf(f, "P6\n%u %u\n255\n", width, height);
    for (unsigned y = 0; y < height; ++y)
    {
        const uint8_t *row = (const uint8_t *)data + y * pitch;
        for (unsigned x = 0; x < width; ++x)
        {
            uint8_t rgb[3];
            if (g_pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888)
            {
                uint32_t p;
                memcpy(&p, row + x * 4, 4);
                rgb[0] = p >> 16; rgb[1] = p >> 8; rgb[2] = p;
            }
            else
            {
                uint16_t p;
                memcpy(&p, row + x * 2, 2);
                rgb[0] = ((p >> 11) & 31) << 3; rgb[1] = ((p >> 5) & 63) << 2; rgb[2] = (p & 31) << 3;
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

static void     VideoRefresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    if (!data || data == RETRO_HW_FRAME_BUFFER_VALID)
        return;
    if (g_shots.count(g_frame))
        SavePpm(data, width, height, pitch, g_dataDir + "/shot_" + std::to_string(g_frame) + ".ppm");
}

static void     AudioSample(int16_t left, int16_t right)
{
    int16_t s[2] = { left, right };
    if (g_audio)
        fwrite(s, 2, 2, g_audio);
}

static size_t   AudioBatch(const int16_t *data, size_t frames)
{
    if (g_audio)
        fwrite(data, 4, frames, g_audio);
    return frames;
}

static void     InputPoll(void) {}

static int16_t  InputState(unsigned port, unsigned device, unsigned /*index*/, unsigned id)
{
    if (port != 0 || device != RETRO_DEVICE_JOYPAD)
        return 0;

    unsigned held = 0;
    for (const Press &p : g_presses)
        if (g_frame >= p.from && g_frame <= p.to)
            held |= p.buttons;

    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return (int16_t)held;
    return (held >> id) & 1;
}

static unsigned ParseButtons(const std::string &list)
{
    static const struct { const char *name; unsigned id; } names[] =
    {
        { "b", RETRO_DEVICE_ID_JOYPAD_B }, { "y", RETRO_DEVICE_ID_JOYPAD_Y },
        { "select", RETRO_DEVICE_ID_JOYPAD_SELECT }, { "start", RETRO_DEVICE_ID_JOYPAD_START },
        { "up", RETRO_DEVICE_ID_JOYPAD_UP }, { "down", RETRO_DEVICE_ID_JOYPAD_DOWN },
        { "left", RETRO_DEVICE_ID_JOYPAD_LEFT }, { "right", RETRO_DEVICE_ID_JOYPAD_RIGHT },
        { "a", RETRO_DEVICE_ID_JOYPAD_A }, { "x", RETRO_DEVICE_ID_JOYPAD_X },
        { "l", RETRO_DEVICE_ID_JOYPAD_L }, { "r", RETRO_DEVICE_ID_JOYPAD_R },
    };

    unsigned mask = 0;
    size_t start = 0;
    while (start <= list.size())
    {
        size_t end = list.find('+', start);
        std::string name = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
        bool found = false;
        for (const auto &n : names)
            if (name == n.name) { mask |= 1u << n.id; found = true; }
        if (!found)
        {
            fprintf(stderr, "unknown button '%s'\n", name.c_str());
            exit(2);
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return mask;
}

template <typename T>
static T        Load(void *core, const char *name)
{
    void *sym = dlsym(core, name);
    if (!sym)
    {
        fprintf(stderr, "core is missing %s\n", name);
        exit(1);
    }
    return reinterpret_cast<T>(sym);
}

int main(int argc, char **argv)
{
    if (argc < 5)
    {
        fprintf(stderr, "usage: %s <core.so> <content> <data dir> <frames> [--press F:T:BTN] [--shot F] [--audio FILE]\n", argv[0]);
        return 2;
    }

    g_dataDir = argv[3];
    const unsigned frames = (unsigned)atoi(argv[4]);

    for (int i = 5; i + 1 < argc; i += 2)
    {
        std::string opt = argv[i], value = argv[i + 1];
        if (opt == "--press")
        {
            unsigned from, to;
            char buttons[128];
            if (sscanf(value.c_str(), "%u:%u:%127s", &from, &to, buttons) != 3)
                return 2;
            g_presses.push_back({from, to, ParseButtons(buttons)});
        }
        else if (opt == "--shot")
            g_shots.insert((unsigned)atoi(value.c_str()));
        else if (opt == "--audio")
            g_audio = fopen(value.c_str(), "wb");
        else
            return 2;
    }

    void *core = dlopen(argv[1], RTLD_NOW);
    if (!core)
    {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }

    Load<void (*)(retro_environment_t)>(core, "retro_set_environment")(Environment);
    Load<void (*)(retro_video_refresh_t)>(core, "retro_set_video_refresh")(VideoRefresh);
    Load<void (*)(retro_audio_sample_t)>(core, "retro_set_audio_sample")(AudioSample);
    Load<void (*)(retro_audio_sample_batch_t)>(core, "retro_set_audio_sample_batch")(AudioBatch);
    Load<void (*)(retro_input_poll_t)>(core, "retro_set_input_poll")(InputPoll);
    Load<void (*)(retro_input_state_t)>(core, "retro_set_input_state")(InputState);
    Load<void (*)(void)>(core, "retro_init")();

    retro_game_info game = {};
    game.path = argv[2];
    if (!Load<bool (*)(const retro_game_info *)>(core, "retro_load_game")(&game))
    {
        fprintf(stderr, "retro_load_game failed\n");
        return 1;
    }

    retro_system_av_info av = {};
    Load<void (*)(retro_system_av_info *)>(core, "retro_get_system_av_info")(&av);
    fprintf(stderr, "[host] %ux%u, %.3f fps, audio %.0f Hz\n", av.geometry.base_width, av.geometry.base_height,
        av.timing.fps, av.timing.sample_rate);

    auto run = Load<void (*)(void)>(core, "retro_run");
    for (g_frame = 0; g_frame < frames; ++g_frame)
    {
        run();
        if (g_frame % 600 == 0)
            fprintf(stderr, "[host] frame %u\n", g_frame);
    }

    Load<void (*)(void)>(core, "retro_unload_game")();
    Load<void (*)(void)>(core, "retro_deinit")();
    if (g_audio)
        fclose(g_audio);
    fprintf(stderr, "[host] done after %u frames\n", frames);
    return 0;
}
