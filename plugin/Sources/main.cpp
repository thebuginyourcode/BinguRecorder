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

    static void     RefreshNames(void)
    {
        Settings &s = Recorder::settings;

        g_screenEntry->Name() = "Screen: " + ScreenNames[s.screens];
        g_fpsEntry->Name() = Utils::Format("Frame rate: %u fps", s.fps);
        g_sizeEntry->Name() = s.halfSize ? "Size: Half (smaller, faster)" : "Size: Full";
        g_indicatorEntry->Name() = s.indicator ? "REC indicator: On" : "REC indicator: Off";
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

        Keyboard kb("Screenshots per second.\n\nOld 3DS: 10 fps (Full) or 20 fps (Half)\nis usually the most it can keep up with.", names);
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
