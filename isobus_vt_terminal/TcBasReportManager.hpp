#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <memory>
#include <mutex>
#include <cstdint>

namespace isobus_vt
{
    // Działka rolna gospodarstwa
    struct FarmParcel
    {
        std::string name = "Pole Wschodnie";
        std::string parcelNumber = "142/3"; // Nr działki ewidencyjnej
        std::string precinct = "Obręb 0012"; // Obręb ewidencyjny
        std::string commune = "Gmina Przykładowa";
        float areaHa = 15.50f; // Powierzchnia działki w ha
        std::string defaultCrop = "Pszenica ozima";
        std::string defaultVariety = "Formacja";
    };

    // Trwały profil gospodarstwa, sprzętu (atesty opryskiwacza) i operatora (szkolenia ŚOR)
    struct FarmProfile
    {
        // 1. Gospodarstwo / Posiadacz
        std::string farmName = "Gospodarstwo Rolne Jan Kowalski";
        std::string farmAddress = "ul. Rolna 15, 00-001 Polskie Pole";
        std::string producerNumberArimr = "067123456";
        std::string commune = "Gmina Przykładowa";
        std::string defaultPrecinct = "Obręb 0012";

        // 2. Sprzęt i Badanie Techniczne (Atest opryskiwacza)
        std::string sprayerModel = "Krukowiak Gollat 2500/21 (ISOBUS)";
        std::string sprayerSerialNo = "KRUK-2022-841";
        std::string sprayerInspectionNo = "BAD-OPR/0412/2024"; // Nr badania technicznego
        std::string sprayerInspectionDate = "2024-05-20";
        std::string sprayerInspectionValidUntil = "2027-05-20"; // Atest ważny 3 lata
        float sprayerBoomWidthM = 21.0f;
        float sprayerTankVolumeL = 2500.0f;
        std::string nozzleType = "Eżektorowe dwustrumieniowe IDTA 120-03";

        // 3. Operator i Zaświadczenie ŚOR
        std::string operatorName = "Jan Kowalski";
        std::string operatorCertificateNo = "ŚOR/WA/2023/00841"; // Nr zaświadczenia o ukończeniu szkolenia
        std::string operatorCertificateValidUntil = "2028-06-15"; // Szkolenie ważne 5 lat

        // 4. Baza Działek Rolnych Gospodarstwa
        std::vector<FarmParcel> parcels;

        bool save_to_file(const std::string &path = "profil_gospodarstwa.json");
        bool load_from_file(const std::string &path = "profil_gospodarstwa.json");
    };

    // Rekord pojedynczego środka z oficjalnego rejestru MRiRW (2953 preparaty)
    struct SorProduct
    {
        std::string name;      // Nazwa handlowa (np. Boxer 800 EC)
        std::string permit;    // Nr zezwolenia MRiRW (np. R-88/2015)
        std::string kind;      // Rodzaj (np. Chwastobójczy / Herbicyd, Fungicyd, Insektycyd, Regulator wzrostu)
        std::string substance; // Substancja czynna (np. prosulfokarb - 800 g)
        std::string producer;  // Producent
        float defaultDose = 1.0f; // Sugerowana dawka
        std::string defaultDoseUnit = "l/ha";
    };

    // Katalog bazy środków ochrony roślin (wyszukiwarka i filtry)
    class SorCatalog
    {
    public:
        static SorCatalog& get_instance();
        SorCatalog();

        bool load_database(const std::string &jsonPath = "sor_database.json");
        std::vector<const SorProduct*> search(const std::string &query, const std::string &kindFilter = "", size_t maxResults = 50) const;
        size_t count() const { return products.size(); }
        const std::vector<SorProduct>& get_all() const { return products; }

    private:
        std::vector<SorProduct> products;
        void load_embedded_fallback();
    };

    // Rekord ewidencji zabiegów środkami ochrony roślin (wymóg od 2026/2027 r.)
    struct SorRecord
    {
        std::string recordId;
        std::string startTimeIso;
        std::string endTimeIso;

        // Działka i lokalizacja
        std::string farmName = "Gospodarstwo Rolne Jan Kowalski";
        std::string fieldName = "Pole Wschodnie";
        std::string cadastralParcel = "142/3"; // Nr działki ewidencyjnej
        std::string cadastralPrecinct = "Obręb 0012"; // Obręb
        std::string commune = "Gmina Przykładowa"; // Gmina
        float totalFieldAreaHa = 15.50f; // Powierzchnia działki
        float appliedAreaHa = 0.0f; // Powierzchnia zabiegu (zliczana na żywo z GPS/TC)

        // Roślina i faza
        std::string cropName = "Pszenica ozima";
        std::string cropVariety = "Formacja";
        std::string bbchStage = "BBCH 31 (pierwsze kolanko)";

        // Środek ochrony roślin
        std::string chemicalName = "Boxer 800 EC";
        std::string permitNumber = "R-88/2015"; // Nr zezwolenia MRiRW
        float dosePerHa = 3.0f; // l/ha lub kg/ha
        std::string doseUnit = "l/ha";
        float waterPerHa = 200.0f; // l/ha cieczy użytkowej
        float totalChemicalUsed = 0.0f; // Łączne zużycie środka (l lub kg)

        // Przyczyna zabiegu
        std::string targetPest = "Miotła zbożowa, przytulia czepna"; // Zwalczany agrofag

        // Warunki atmosferyczne
        float temperatureC = 16.5f;
        float windSpeedMs = 2.1f; // Prędkość wiatru m/s (dopuszczalna < 4 m/s)
        float humidityPct = 65.0f;
        std::string weatherConditions = "Słonecznie, lekki wiatr";

        // Osoba wykonująca zabieg
        std::string operatorName = "Jan Kowalski";
        std::string operatorCertificateNo = "ŚOR/WA/2023/00841"; // Nr zaświadczenia o ukończeniu szkolenia

        // Sprzęt
        std::string sprayerModel = "Krukowiak Gollat 2500/21 (ISOBUS)";
        std::string sprayerInspectionNo = "BAD-OPR/0412/2024"; // Badanie techniczne opryskiwacza
        std::string sprayerInspectionValidUntil = "2027-05-20"; // Ważność atestu

        // Status
        bool isCompleted = false;
    };

    // Sesja zadania TC-BAS (Task Controller Basic wg ISO 11783-10)
    struct TcTaskSession
    {
        std::string taskId = "TSK1";
        std::string taskName = "Zabieg polowy ŚOR";
        std::string customerName = "Gospodarstwo Rolne Jan Kowalski";
        std::string farmName = "Gospodarstwo Główne";
        std::string fieldName = "Działka 142/3";
        std::string workerName = "Jan Kowalski";
        std::string implementName = "Krukowiak Gollat 2500/21 (ISOBUS)";
        float implementWidthMeters = 28.0f; // Szerokość robocza w metrach
        int implementSectionsCount = 8;     // Liczba sekcji roboczych

        // Czasy
        std::chrono::system_clock::time_point startTime;
        std::chrono::system_clock::time_point stopTime;
        float elapsedWorkSeconds = 0.0f;
        bool isRunning = false;
        bool isPaused = false;

        // Liczniki na żywo (Totals)
        float workedAreaHa = 0.0f;
        float workedDistanceKm = 0.0f;
        float avgSpeedKph = 0.0f;
        float appliedProductTotal = 0.0f; // Zużycie preparatu (l)

        // Dane do ewidencji ŚOR
        SorRecord sorData;
    };

    class TcBasReportManager
    {
    public:
        static TcBasReportManager& get_instance();

        TcBasReportManager();
        ~TcBasReportManager();

        // Zarządzanie profilem stałym gospodarstwa
        FarmProfile& get_farm_profile() { return farmProfile; }
        const FarmProfile& get_farm_profile() const { return farmProfile; }
        void save_farm_profile() { farmProfile.save_to_file(); }
        void apply_farm_profile_to_current();

        // Wybór działki z bazy profilu
        void select_parcel_for_task(size_t parcelIndex);

        // Wybór środka z oficjalnego katalogu MRiRW
        void select_chemical_from_catalog(const SorProduct &product);

        // Zarządzanie bieżącym zadaniem
        void start_task(const std::string &taskName, const std::string &fieldName, float widthM);
        void pause_task();
        void resume_task();
        void stop_task();
        bool is_task_active() const;
        bool is_task_paused() const;

        // Akumulacja danych roboczych (wywoływana cyklicznie z wątku głównego / TC)
        void update_work_data(float speedKph, float activeSectionRatio, float dtSeconds);

        // Dostęp do aktywnego zadania
        TcTaskSession& get_current_task() { return currentTask; }
        const TcTaskSession& get_current_task() const { return currentTask; }

        // Historia zakończonych zadań / zabiegów
        const std::vector<TcTaskSession>& get_task_history() const { return taskHistory; }
        void clear_history();

        // Eksport danych ISO-XML (ISO 11783-10 TASKDATA.XML)
        bool export_taskdata_xml(const std::string &outputDirectory = "TASKDATA", std::string *outFilePath = nullptr);

        // Raporty Ewidencji ŚOR (Wymóg prawny od 2027)
        bool export_sor_html_report(const SorRecord &record, const std::string &outputDirectory = "Raporty_SOR", std::string *outFilePath = nullptr);
        bool export_sor_csv_register(const std::string &outputDirectory = "Raporty_SOR", std::string *outFilePath = nullptr);

        // Bezpośrednia synchronizacja danych z AgOpenGPS (Profil maszyny, aktywne pole, koordynaty, sekcje)
        bool scan_and_sync_agopengps();
        void set_field_name_from_aog(const std::string &name);
        void set_last_aog_sync_info(const std::string &info) { std::lock_guard<std::mutex> lk(dataMutex); lastAogSyncInfo = info; }
        const std::string& get_last_aog_sync_info() const { return lastAogSyncInfo; }

        // Metody pomocnicze
        static std::string format_time_hhmmss(float seconds);
        static std::string get_current_iso_time();
        void set_predefined_sor(int templateIndex);

    private:
        mutable std::mutex dataMutex;
        FarmProfile farmProfile;
        TcTaskSession currentTask;
        std::vector<TcTaskSession> taskHistory;
        std::string lastAogSyncInfo = "Oczekiwanie na dane z AgOpenGPS";
        std::string lastExportedXmlPath;
        std::string lastExportedHtmlPath;

        void load_history_from_disk();
        void save_history_to_disk();
    };

} // namespace isobus_vt
