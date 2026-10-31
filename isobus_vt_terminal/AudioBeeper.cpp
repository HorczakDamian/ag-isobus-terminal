#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "AudioBeeper.hpp"
#include "TerminalSettings.hpp"

namespace isobus_vt
{
    AudioBeeper& AudioBeeper::get_instance()
    {
        static AudioBeeper instance;
        return instance;
    }

    AudioBeeper::AudioBeeper()
    {
        workerThread = std::thread(&AudioBeeper::worker_loop, this);
    }

    AudioBeeper::~AudioBeeper()
    {
        shutdown();
    }

    void AudioBeeper::shutdown()
    {
        if (isRunning.exchange(false))
        {
            cv.notify_all();
            if (workerThread.joinable())
            {
                workerThread.join();
            }
        }
    }

    void AudioBeeper::play_touch_click()
    {
        if (!TerminalSettings::get_instance().get_config().touchBeepEnabled) return;

        std::uint64_t now = GetTickCount64();
        if (now - lastClickTick < 50) return; // Zapobieganie zalewaniu dźwiękami
        lastClickTick = now;

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (toneQueue.size() < 4)
            {
                toneQueue.push(AudioTone::TouchClick);
            }
        }
        cv.notify_one();
    }

    void AudioBeeper::play_soft_key_click()
    {
        if (!TerminalSettings::get_instance().get_config().touchBeepEnabled) return;

        std::uint64_t now = GetTickCount64();
        if (now - lastClickTick < 50) return;
        lastClickTick = now;

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (toneQueue.size() < 4)
            {
                toneQueue.push(AudioTone::SoftKeyClick);
            }
        }
        cv.notify_one();
    }

    void AudioBeeper::play_alarm_tone()
    {
        if (TerminalSettings::get_instance().get_config().alarmSoundMuted) return;

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (toneQueue.size() < 3)
            {
                toneQueue.push(AudioTone::AlarmPulse);
            }
        }
        cv.notify_one();
    }

    void AudioBeeper::play_ack_chime()
    {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            // Wyczyszczenie oczekujących tonów alarmu po zatwierdzeniu ACK
            while (!toneQueue.empty()) toneQueue.pop();
            toneQueue.push(AudioTone::AckChime);
        }
        cv.notify_one();
    }

    void AudioBeeper::play_error_buzz()
    {
        if (!TerminalSettings::get_instance().get_config().touchBeepEnabled) return;

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (toneQueue.size() < 3)
            {
                toneQueue.push(AudioTone::ErrorBuzz);
            }
        }
        cv.notify_one();
    }

    void AudioBeeper::update_alarm_loop(bool isAlarmActive)
    {
        if (!isAlarmActive) return;
        if (TerminalSettings::get_instance().get_config().alarmSoundMuted) return;

        std::uint64_t now = GetTickCount64();
        if (now - lastAlarmTick >= 1500) // Powtarzanie alarmu co 1.5 sekundy wg normy ISO 11783-6
        {
            lastAlarmTick = now;
            play_alarm_tone();
        }
    }

    void AudioBeeper::worker_loop()
    {
        while (isRunning)
        {
            AudioTone tone;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                cv.wait(lock, [this]() { return !isRunning || !toneQueue.empty(); });
                if (!isRunning) break;

                tone = toneQueue.front();
                toneQueue.pop();
            }

            execute_tone(tone);
        }
    }

    void AudioBeeper::execute_tone(AudioTone tone)
    {
        switch (tone)
        {
            case AudioTone::TouchClick:
                Beep(2400, 16);
                break;

            case AudioTone::SoftKeyClick:
                Beep(2100, 24);
                break;

            case AudioTone::AlarmPulse:
                Beep(980, 80);
                Sleep(40);
                Beep(980, 80);
                Sleep(40);
                Beep(980, 110);
                break;

            case AudioTone::AckChime:
                Beep(1300, 50);
                Sleep(20);
                Beep(1900, 80);
                break;

            case AudioTone::ErrorBuzz:
                Beep(420, 120);
                break;
        }
    }
}
