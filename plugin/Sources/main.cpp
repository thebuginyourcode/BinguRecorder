#include <3ds.h>
#include <CTRPluginFramework.hpp>

#include "Recorder.hpp"

#include <string>
#include <vector>

namespace CTRPluginFramework
{
    using namespace Bingu;

    static const std::vector<std::string>   ScreenNames = { "Top screen", "Bottom screen", "Both screens" };
    static const std::vector<u8>            FpsChoices = { 5, 10, 15, 20, 30 };

    static MenuEntry   *g_screenEntry;
    static MenuEntry   *g_fpsEntry;
    static MenuEntry   *g_sizeEntry;
    static MenuEntry   *g_indicatorEntry;
    static MenuEntry   *g_compressEntry;
    static MenuEntry   *g_qualityEntry;
    static MenuEntry   *g_soundEntry;

    static const std::vector<std::string>   SoundNames =
    {
        "Off",
        "On (stereo, ~2 MB/min)",
        "Small (mono, ~0.5 MB/min)",
    };

    struct QualityChoice
    {
        const char *name;
        u8          value;
    };

    static const QualityChoice  QualityChoices[] =
    {
        { "Low", 50 }, { "Medium", 70 }, { "High", 85 }, { "Max", 95 },
    };

    static std::string  QualityName(u8 value)
    {
        for (const QualityChoice &q : QualityChoices)
        {
            if (q.value == value)
                return q.name;
        }
        return Utils::Format("%u", value);
    }

    static void     RefreshNames(void)
    {
        Settings &s = Recorder::settings;

        g_screenEntry->Name() = "Screen: " + ScreenNames[s.screens];
        g_fpsEntry->Name() = Utils::Format("Frame rate: %u fps", s.fps);
        g_sizeEntry->Name() = s.halfSize ? "Size: Half (smaller, faster)" : "Size: Full";
        g_indicatorEntry->Name() = s.indicator ? "REC indicator: On" : "REC indicator: Off";
        g_compressEntry->Name() = s.compress ? "Compression: On (small files)" : "Compression: Off (huge files)";
        g_qualityEntry->Name() = "Quality: " + QualityName(s.quality);
        g_soundEntry->Name() = "Sound: " + SoundNames[s.audio];
    }

    static bool     ChangeSetting(void)
    {
        if (Recorder::IsRecording())
        {
            MessageBox("Stop the recording before changing settings.")();
            return false;
        }
        return true;
    }

    static void     SettingsChanged(void)
    {
        Recorder::settings.Save();
        RefreshNames();
    }

    // Runs every frame: the start/stop hotkey
    static void     HotkeyWatcher(MenuEntry *entry)
    {
        if (entry->Hotkeys[0].IsPressed())
            Recorder::Toggle();
    }

    static void     ToggleFromMenu(MenuEntry *entry)
    {
        bool recording = Recorder::IsRecording();

        if (recording)
            Recorder::Stop();
        else
            Recorder::Start();

        MessageBox(recording ? "Recording stopped, the video is being saved." :
            "Recording started!\nClose the menu to continue playing.\n\nPress " +
            entry->Hotkeys[0].ToString() + " to stop.")();
    }

    static void     ChooseScreen(MenuEntry *)
    {
        if (!ChangeSetting())
            return;

        Keyboard kb("Which screen should be recorded?", ScreenNames);
        int choice = kb.Open();

        if (choice >= 0)
        {
            Recorder::settings.screens = (u8)choice;
            SettingsChanged();
        }
    }

    static void     ChooseFps(MenuEntry *)
    {
        if (!ChangeSetting())
            return;

        std::vector<std::string> names;
        for (u8 fps : FpsChoices)
            names.push_back(Utils::Format("%u fps", fps));

        Keyboard kb("Screenshots per second.\n\nOn an Old 3DS, about 10 fps (Full) or\n20-30 fps (Half) is usually the most it\ncan keep up with. Frames it can't keep up\nwith are skipped, the game never slows down.", names);
        int choice = kb.Open();

        if (choice >= 0)
        {
            Recorder::settings.fps = FpsChoices[choice];
            SettingsChanged();
        }
    }

    static void     ChooseSize(MenuEntry *)
    {
        if (!ChangeSetting())
            return;

        Keyboard kb("Video size", { "Full (400x240)", "Half (200x120) - allows higher frame rates" });
        int choice = kb.Open();

        if (choice >= 0)
        {
            Recorder::settings.halfSize = (u8)choice;
            SettingsChanged();
        }
    }

    static void     ToggleCompression(MenuEntry *)
    {
        if (!ChangeSetting())
            return;

        Keyboard kb("Compression",
        {
            "On: JPEG frames (~10x smaller files)",
            "Off: raw bitmaps (lossless, ~115 MB/min)",
        });
        int choice = kb.Open();

        if (choice >= 0)
        {
            Recorder::settings.compress = choice == 0;
            SettingsChanged();
        }
    }

    static void     ChooseQuality(MenuEntry *)
    {
        if (!ChangeSetting())
            return;

        std::vector<std::string> names;
        for (const QualityChoice &q : QualityChoices)
            names.push_back(q.name);

        Keyboard kb("JPEG quality (when compression is on).\nHigher looks better but makes bigger files.", names);
        int choice = kb.Open();

        if (choice >= 0)
        {
            Recorder::settings.quality = QualityChoices[choice].value;
            SettingsChanged();
        }
    }

    static void     ChooseSound(MenuEntry *)
    {
        if (!ChangeSetting())
            return;

        Keyboard kb("Record the game's sound?\n\nSound is compressed (IMA ADPCM) to keep\nfiles small.", SoundNames);
        int choice = kb.Open();

        if (choice >= 0)
        {
            Recorder::settings.audio = (u8)choice;
            SettingsChanged();
        }
    }

    static void     ToggleIndicator(MenuEntry *)
    {
        Recorder::settings.indicator ^= 1;
        SettingsChanged();
    }

    void    PatchProcess(FwkSettings &settings)
    {
        settings.WaitTimeToBoot = Seconds(3);
    }

    int     main(void)
    {
        PluginMenu *menu = new PluginMenu("BinguRecorder", 1, 0, 0,
            "Records the game screen to the SD card.\n\n"
            "Videos are saved as .avi files in\nsd:/BinguRecorder/");

        menu->SynchronizeWithFrame(true);
        menu->ShowWelcomeMessage(false);

        Recorder::Initialize();

        MenuEntry *hotkey = new MenuEntry("Start / stop recording", HotkeyWatcher, ToggleFromMenu,
            "Select this entry to start or stop recording.\n\n"
            "While playing, press the hotkey (default L + R + Down)\nto start and stop. "
            "You can change the hotkey with the\ncontroller icon below.");
        hotkey->Hotkeys += Hotkey(Key::L | Key::R | Key::DPadDown, "Start / stop recording");
        *menu += hotkey;
        hotkey->Enable();

        MenuFolder *folder = new MenuFolder("Settings");
        g_screenEntry = new MenuEntry("", nullptr, ChooseScreen, "Record the top screen, the bottom screen, or both stacked on top of each other.");
        g_fpsEntry = new MenuEntry("", nullptr, ChooseFps, "How many screenshots are taken per second.");
        g_sizeEntry = new MenuEntry("", nullptr, ChooseSize, "Half size uses 4x less data, so the Old 3DS can record at a higher frame rate.");
        g_indicatorEntry = new MenuEntry("", nullptr, ToggleIndicator, "Shows a blinking REC timer on the screen that is NOT being recorded.");
        g_compressEntry = new MenuEntry("", nullptr, ToggleCompression, "Saves every frame as a JPEG inside the video: about 10x smaller files. Compressing only uses CPU time the game leaves free.");
        g_qualityEntry = new MenuEntry("", nullptr, ChooseQuality, "Image quality of the compressed frames.");
        g_soundEntry = new MenuEntry("", nullptr, ChooseSound, "Records what comes out of the speakers. Small saves mono sound at half the sample rate.");
        *folder += g_soundEntry;
        *folder += g_compressEntry;
        *folder += g_qualityEntry;
        *folder += g_screenEntry;
        *folder += g_fpsEntry;
        *folder += g_sizeEntry;
        *folder += g_indicatorEntry;
        *menu += folder;
        RefreshNames();

        menu->Run();

        Recorder::Stop(true);
        delete menu;
        return 0;
    }
}
