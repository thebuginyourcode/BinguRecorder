#ifndef BINGU_RECORDER_HPP
#define BINGU_RECORDER_HPP

#include <3ds.h>

namespace Bingu
{
    enum class CaptureScreens : u8
    {
        Top = 0,
        Bottom,
        Both,
    };

    struct Settings
    {
        u8      version{1};
        u8      screens{(u8)CaptureScreens::Top};
        u8      fps{10};
        u8      halfSize{0};
        u8      indicator{1};

        void    Load(void);
        void    Save(void) const;
    };

    // Grabs the game's framebuffer a few times per second and streams the
    // frames into an AVI file on the SD card (/BinguRecorder/*.avi).
    //
    // Capturing happens on the game's thread (inside the CTRPF OSD hook, right
    // when a frame is presented) and is just a pixel copy into a small ring of
    // buffers. A background thread does the slow part: writing to the SD card.
    namespace Recorder
    {
        extern Settings     settings;

        void    Initialize(void);
        void    Start(void);
        // Stops the recording; when `wait` is true, blocks until the file is
        // fully written (used when the game is about to close).
        void    Stop(bool wait = false);
        void    Toggle(void);
        bool    IsRecording(void);
    }
}

#endif
