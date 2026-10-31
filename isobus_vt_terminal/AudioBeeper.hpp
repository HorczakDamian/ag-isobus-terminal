#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>

namespace isobus_vt
{
    enum class AudioTone
    {
        TouchClick,      // Delikatne kliknięcie ekranu dotykowego (2400 Hz, 16 ms)
        SoftKeyClick,    // Kliknięcie fizycznego/wirtualnego Soft Key (2100 Hz, 24 ms)
        AlarmPulse,      // Potrójny alarm ISOBUS ISO 11783-6 (950 Hz, 3 impulsy)
        AckChime,        // Dźwięk potwierdzenia alarmu (1300 Hz -> 1900 Hz)
        ErrorBuzz        // Dźwięk błędu/odrzucenia wartości (400 Hz, 120 ms)
    };

    class AudioBeeper
    {
    public:
        static AudioBeeper& get_instance();

        // Metody odtwarzania dźwięków
        void play_touch_click();
        void play_soft_key_click();
        void play_alarm_tone();
        void play_ack_chime();
        void play_error_buzz();

        // Pętla aktualizacji alarmu (wywoływana co klatkę z głównego wątku UI)
        void update_alarm_loop(bool isAlarmActive);

        // Zatrzymanie wątku dźwiękowego przy zamykaniu aplikacji
        void shutdown();

    private:
        AudioBeeper();
        ~AudioBeeper();

        AudioBeeper(const AudioBeeper&) = delete;
        AudioBeeper& operator=(const AudioBeeper&) = delete;

        void worker_loop();
        void execute_tone(AudioTone tone);

        std::thread workerThread;
        std::mutex queueMutex;
        std::condition_variable cv;
        std::queue<AudioTone> toneQueue;
        std::atomic<bool> isRunning{ true };

        std::uint64_t lastAlarmTick{ 0 };
        std::uint64_t lastClickTick{ 0 };
    };
}
