// TcBasReportManager.cpp
// Full implementation of TC-BAS (Task Controller Basic - ISO 11783-10),
// electronic plant protection register (Ewidencja ŚOR 2027),
// persistent Farm Profile (Atesty opryskiwacza, zaświadczenia ŚOR)
// and official MRiRW plant protection products catalog (2953 preparaty).

#include "TcBasReportManager.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <filesystem>
#include <algorithm>
#include <cctype>

namespace isobus_vt
{
    // Pomocnicza konwersja do małych liter dla wyszukiwarki
    static std::string to_lower_utf8(const std::string &s)
    {
        std::string res;
        res.reserve(s.size());
        for (unsigned char c : s)
        {
            // Podstawowa obsługa ASCII
            if (c >= 'A' && c <= 'Z') res += static_cast<char>(c + 32);
            else res += static_cast<char>(c);
        }
        return res;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // FarmProfile - Trwały profil gospodarstwa, sprzętu i uprawnień operatora
    // ─────────────────────────────────────────────────────────────────────────────
    bool FarmProfile::save_to_file(const std::string &path)
    {
        std::ofstream f(path, std::ios::trunc);
        if (!f.is_open()) return false;

        f << "{\n";
        f << "  \"farmName\": \"" << farmName << "\",\n";
        f << "  \"farmAddress\": \"" << farmAddress << "\",\n";
        f << "  \"producerNumberArimr\": \"" << producerNumberArimr << "\",\n";
        f << "  \"commune\": \"" << commune << "\",\n";
        f << "  \"defaultPrecinct\": \"" << defaultPrecinct << "\",\n";
        f << "  \"sprayerModel\": \"" << sprayerModel << "\",\n";
        f << "  \"sprayerSerialNo\": \"" << sprayerSerialNo << "\",\n";
        f << "  \"sprayerInspectionNo\": \"" << sprayerInspectionNo << "\",\n";
        f << "  \"sprayerInspectionDate\": \"" << sprayerInspectionDate << "\",\n";
        f << "  \"sprayerInspectionValidUntil\": \"" << sprayerInspectionValidUntil << "\",\n";
        f << "  \"sprayerBoomWidthM\": " << sprayerBoomWidthM << ",\n";
        f << "  \"sprayerTankVolumeL\": " << sprayerTankVolumeL << ",\n";
        f << "  \"nozzleType\": \"" << nozzleType << "\",\n";
        f << "  \"operatorName\": \"" << operatorName << "\",\n";
        f << "  \"operatorCertificateNo\": \"" << operatorCertificateNo << "\",\n";
        f << "  \"operatorCertificateValidUntil\": \"" << operatorCertificateValidUntil << "\",\n";
        f << "  \"parcels\": [\n";
        for (size_t i = 0; i < parcels.size(); ++i)
        {
            const auto &p = parcels[i];
            f << "    {\n";
            f << "      \"name\": \"" << p.name << "\",\n";
            f << "      \"parcelNumber\": \"" << p.parcelNumber << "\",\n";
            f << "      \"precinct\": \"" << p.precinct << "\",\n";
            f << "      \"commune\": \"" << p.commune << "\",\n";
            f << "      \"areaHa\": " << p.areaHa << ",\n";
            f << "      \"defaultCrop\": \"" << p.defaultCrop << "\",\n";
            f << "      \"defaultVariety\": \"" << p.defaultVariety << "\"\n";
            f << "    }" << (i + 1 < parcels.size() ? ",\n" : "\n");
        }
        f << "  ]\n";
        f << "}\n";
        return true;
    }

    bool FarmProfile::load_from_file(const std::string &path)
    {
        if (!std::filesystem::exists(path))
        {
            // Domyślne przykładowe działki gospodarstwa
            parcels.clear();
            FarmParcel p1;
            p1.name = "Pole Wschodnie - Główny łan";
            p1.parcelNumber = "142/3";
            p1.precinct = "Obręb 0012";
            p1.commune = commune;
            p1.areaHa = 18.50f;
            p1.defaultCrop = "Pszenica ozima";
            p1.defaultVariety = "Formacja";
            parcels.push_back(p1);

            FarmParcel p2;
            p2.name = "Działka Leśna";
            p2.parcelNumber = "88/1";
            p2.precinct = "Obręb 0012";
            p2.commune = commune;
            p2.areaHa = 12.20f;
            p2.defaultCrop = "Rzepak ozimy";
            p2.defaultVariety = "LG Aviron";
            parcels.push_back(p2);

            FarmParcel p3;
            p3.name = "Kawałek przy Szosie";
            p3.parcelNumber = "215/4";
            p3.precinct = "Obręb 0014";
            p3.commune = commune;
            p3.areaHa = 9.80f;
            p3.defaultCrop = "Kukurydza";
            p3.defaultVariety = "Pioneer P8834";
            parcels.push_back(p3);

            save_to_file(path);
            return true;
        }

        std::ifstream f(path);
        if (!f.is_open()) return false;

        std::stringstream buffer;
        buffer << f.rdbuf();
        std::string json = buffer.str();

        auto extractStr = [&](const std::string &key) -> std::string {
            std::string search = "\"" + key + "\": \"";
            size_t pos = json.find(search);
            if (pos == std::string::npos) return "";
            size_t start = pos + search.size();
            size_t end = json.find("\"", start);
            if (end == std::string::npos) return "";
            return json.substr(start, end - start);
        };

        auto extractFloat = [&](const std::string &key) -> float {
            std::string search = "\"" + key + "\": ";
            size_t pos = json.find(search);
            if (pos == std::string::npos) return 0.0f;
            size_t start = pos + search.size();
            try { return std::stof(json.substr(start)); } catch (...) { return 0.0f; }
        };

        std::string s;
        if (!(s = extractStr("farmName")).empty()) farmName = s;
        if (!(s = extractStr("farmAddress")).empty()) farmAddress = s;
        if (!(s = extractStr("producerNumberArimr")).empty()) producerNumberArimr = s;
        if (!(s = extractStr("commune")).empty()) commune = s;
        if (!(s = extractStr("defaultPrecinct")).empty()) defaultPrecinct = s;
        if (!(s = extractStr("sprayerModel")).empty()) sprayerModel = s;
        if (!(s = extractStr("sprayerSerialNo")).empty()) sprayerSerialNo = s;
        if (!(s = extractStr("sprayerInspectionNo")).empty()) sprayerInspectionNo = s;
        if (!(s = extractStr("sprayerInspectionDate")).empty()) sprayerInspectionDate = s;
        if (!(s = extractStr("sprayerInspectionValidUntil")).empty()) sprayerInspectionValidUntil = s;
        if (float val = extractFloat("sprayerBoomWidthM")) sprayerBoomWidthM = val;
        if (float val = extractFloat("sprayerTankVolumeL")) sprayerTankVolumeL = val;
        if (!(s = extractStr("nozzleType")).empty()) nozzleType = s;
        if (!(s = extractStr("operatorName")).empty()) operatorName = s;
        if (!(s = extractStr("operatorCertificateNo")).empty()) operatorCertificateNo = s;
        if (!(s = extractStr("operatorCertificateValidUntil")).empty()) operatorCertificateValidUntil = s;

        // Proste odczytanie działek
        size_t pArrPos = json.find("\"parcels\": [");
        if (pArrPos != std::string::npos)
        {
            parcels.clear();
            size_t curPos = pArrPos;
            while (true)
            {
                size_t objStart = json.find("{", curPos);
                if (objStart == std::string::npos) break;
                size_t objEnd = json.find("}", objStart);
                if (objEnd == std::string::npos) break;

                std::string block = json.substr(objStart, objEnd - objStart + 1);
                auto getBlockStr = [&](const std::string &k) -> std::string {
                    std::string sch = "\"" + k + "\": \"";
                    size_t p = block.find(sch);
                    if (p == std::string::npos) return "";
                    size_t st = p + sch.size();
                    size_t ed = block.find("\"", st);
                    if (ed == std::string::npos) return "";
                    return block.substr(st, ed - st);
                };
                auto getBlockFloat = [&](const std::string &k) -> float {
                    std::string sch = "\"" + k + "\": ";
                    size_t p = block.find(sch);
                    if (p == std::string::npos) return 0.0f;
                    size_t st = p + sch.size();
                    try { return std::stof(block.substr(st)); } catch (...) { return 0.0f; }
                };

                FarmParcel p;
                p.name = getBlockStr("name");
                p.parcelNumber = getBlockStr("parcelNumber");
                p.precinct = getBlockStr("precinct");
                p.commune = getBlockStr("commune");
                p.areaHa = getBlockFloat("areaHa");
                p.defaultCrop = getBlockStr("defaultCrop");
                p.defaultVariety = getBlockStr("defaultVariety");

                if (!p.name.empty() || !p.parcelNumber.empty())
                    parcels.push_back(p);

                curPos = objEnd + 1;
                size_t arrEnd = json.find("]", curPos);
                if (curPos >= arrEnd && arrEnd != std::string::npos) break;
            }
        }

        return true;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // SorCatalog - Baza 2953 oficjalnych środków ochrony roślin MRiRW
    // ─────────────────────────────────────────────────────────────────────────────
    SorCatalog& SorCatalog::get_instance()
    {
        static SorCatalog inst;
        return inst;
    }

    SorCatalog::SorCatalog()
    {
        if (!load_database("sor_database.json"))
        {
            if (!load_database("../sor_database.json"))
            {
                load_embedded_fallback();
            }
        }
    }

    bool SorCatalog::load_database(const std::string &jsonPath)
    {
        if (!std::filesystem::exists(jsonPath)) return false;

        std::ifstream file(jsonPath, std::ios::binary);
        if (!file.is_open()) return false;

        std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        products.clear();
        products.reserve(3000);

        // Lekki, szybki parser JSON dla tablicy obiektów
        size_t pos = 0;
        while (pos < json.size())
        {
            size_t objStart = json.find('{', pos);
            if (objStart == std::string::npos) break;
            size_t objEnd = json.find('}', objStart);
            if (objEnd == std::string::npos) break;

            std::string block = json.substr(objStart, objEnd - objStart + 1);

            auto getField = [&](const std::string &k) -> std::string {
                std::string sch = "\"" + k + "\": \"";
                size_t p = block.find(sch);
                if (p == std::string::npos) return "";
                size_t st = p + sch.size();
                size_t ed = st;
                while (ed < block.size())
                {
                    if (block[ed] == '"' && block[ed - 1] != '\\') break;
                    ed++;
                }
                std::string val = block.substr(st, ed - st);
                // Zamień \n na spację
                size_t nPos = 0;
                while ((nPos = val.find("\\n", nPos)) != std::string::npos)
                {
                    val.replace(nPos, 2, " ");
                }
                return val;
            };

            std::string name = getField("name");
            std::string permit = getField("permit");
            std::string kind = getField("kind");
            std::string substance = getField("substance");
            std::string producer = getField("producer");

            if (!name.empty() && !permit.empty())
            {
                SorProduct prod;
                prod.name = name;
                prod.permit = permit;
                prod.kind = kind;
                prod.substance = substance;
                prod.producer = producer;

                // Sugerowana dawka zależnie od rodzaju
                if (kind.find("Chwast") != std::string::npos) { prod.defaultDose = 2.5f; }
                else if (kind.find("Fungicyd") != std::string::npos) { prod.defaultDose = 1.0f; }
                else if (kind.find("Insektycyd") != std::string::npos) { prod.defaultDose = 0.15f; }
                else if (kind.find("Regulator") != std::string::npos) { prod.defaultDose = 1.0f; }
                else { prod.defaultDose = 1.0f; }

                products.push_back(prod);
            }
            pos = objEnd + 1;
        }

        return !products.empty();
    }

    void SorCatalog::load_embedded_fallback()
    {
        products = {
            { "Boxer 800 EC", "R-88/2015", "Chwastobójczy", "prosulfokarb - 800 g", "Syngenta Crop Protection", 3.0f, "l/ha" },
            { "Roundup 360 Plus", "R-71/2014", "Chwastobójczy", "glifosat - 360 g", "Bayer CropScience", 3.0f, "l/ha" },
            { "Delaro 325 SC", "R-18/2016wu", "Fungicyd", "protiokonazol - 175 g, trifloksystrobina - 150 g", "Bayer CropScience", 1.0f, "l/ha" },
            { "Elatus Era", "R-229/2017", "Fungicyd", "benzowindyflupyr - 75 g, protiokonazol - 150 g", "Syngenta Crop Protection", 0.8f, "l/ha" },
            { "Karate Zeon 050 CS", "R-15/2015", "Insektycyd", "lambda-cyhalotryna - 50 g", "Syngenta Crop Protection", 0.12f, "l/ha" },
            { "Mustang 306 SE", "R-53/2010", "Chwastobójczy", "2,4-D - 300 g, florasulam - 6,25 g", "Corteva Agriscience", 0.6f, "l/ha" },
            { "Puma Uniwersal 069 EW", "R-107/2014", "Chwastobójczy", "fenoksaprop-P etylu - 69 g", "Bayer CropScience", 1.0f, "l/ha" },
            { "Lumax 537.5 SE", "R-33/2019", "Chwastobójczy", "mezotrion - 37.5 g, s-metolachlor - 312.5 g, terbutylazyna - 187.5 g", "Syngenta", 3.5f, "l/ha" },
            { "Caryx 240 SL", "R-71/2017", "Regulator wzrostu, Fungicyd", "chlorek mepikwatu - 210 g, metkonazol - 30 g", "BASF SE", 1.0f, "l/ha" }
        };
    }

    std::vector<const SorProduct*> SorCatalog::search(const std::string &query, const std::string &kindFilter, size_t maxResults) const
    {
        std::vector<const SorProduct*> res;
        std::string q = to_lower_utf8(query);
        std::string kFilter = to_lower_utf8(kindFilter);

        for (const auto &p : products)
        {
            if (res.size() >= maxResults) break;

            if (!kFilter.empty() && kFilter != "wszystkie")
            {
                std::string kLower = to_lower_utf8(p.kind);
                if (kLower.find(kFilter) == std::string::npos)
                    continue;
            }

            if (q.empty())
            {
                res.push_back(&p);
            }
            else
            {
                std::string nLower = to_lower_utf8(p.name);
                std::string sLower = to_lower_utf8(p.substance);
                std::string permLower = to_lower_utf8(p.permit);

                if (nLower.find(q) != std::string::npos ||
                    sLower.find(q) != std::string::npos ||
                    permLower.find(q) != std::string::npos)
                {
                    res.push_back(&p);
                }
            }
        }
        return res;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TcBasReportManager - Centralny menedżer
    // ─────────────────────────────────────────────────────────────────────────────
    TcBasReportManager& TcBasReportManager::get_instance()
    {
        static TcBasReportManager instance;
        return instance;
    }

    TcBasReportManager::TcBasReportManager()
    {
        // 1. Wczytaj trwały profil gospodarstwa (Atesty, zaświadczenia, działki)
        farmProfile.load_from_file("profil_gospodarstwa.json");

        // 2. Wczytaj bazę 2953 oficjalnych środków MRiRW
        SorCatalog::get_instance();

        // 3. Zaaplikuj stałe dane profilu do zadania i ewidencji
        apply_farm_profile_to_current();

        // 3b. Bezpośrednio zsynchronizuj z AgOpenGPS (wykryj aktywne pole, maszynę z ToolProfiles)
        scan_and_sync_agopengps();

        // 4. Załaduj historię zadań z dysku
        load_history_from_disk();
    }

    TcBasReportManager::~TcBasReportManager()
    {
        save_farm_profile();
        save_history_to_disk();
    }

    void TcBasReportManager::apply_farm_profile_to_current()
    {
        std::lock_guard<std::mutex> lock(dataMutex);

        currentTask.customerName = farmProfile.farmName;
        currentTask.farmName = farmProfile.farmName;
        currentTask.workerName = farmProfile.operatorName;
        currentTask.implementName = farmProfile.sprayerModel;
        if (farmProfile.sprayerBoomWidthM > 0.0f)
            currentTask.implementWidthMeters = farmProfile.sprayerBoomWidthM;

        auto &s = currentTask.sorData;
        s.farmName = farmProfile.farmName;
        s.commune = farmProfile.commune;
        s.operatorName = farmProfile.operatorName;
        s.operatorCertificateNo = farmProfile.operatorCertificateNo;
        s.sprayerModel = farmProfile.sprayerModel;
        s.sprayerInspectionNo = farmProfile.sprayerInspectionNo;
        s.sprayerInspectionValidUntil = farmProfile.sprayerInspectionValidUntil;

        // Jeśli są zapisane działki w profilu, ustaw pierwszą domyślnie
        if (!farmProfile.parcels.empty() && s.fieldName == "Pole Północne")
        {
            const auto &p = farmProfile.parcels[0];
            currentTask.fieldName = p.name;
            s.fieldName = p.name;
            s.cadastralParcel = p.parcelNumber;
            s.cadastralPrecinct = p.precinct;
            s.totalFieldAreaHa = p.areaHa;
            s.cropName = p.defaultCrop;
            s.cropVariety = p.defaultVariety;
        }
    }

    void TcBasReportManager::select_parcel_for_task(size_t parcelIndex)
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        if (parcelIndex >= farmProfile.parcels.size()) return;

        const auto &p = farmProfile.parcels[parcelIndex];
        currentTask.fieldName = p.name;

        auto &s = currentTask.sorData;
        s.fieldName = p.name;
        s.cadastralParcel = p.parcelNumber;
        s.cadastralPrecinct = p.precinct;
        if (!p.commune.empty()) s.commune = p.commune;
        s.totalFieldAreaHa = p.areaHa;
        if (!p.defaultCrop.empty()) s.cropName = p.defaultCrop;
        if (!p.defaultVariety.empty()) s.cropVariety = p.defaultVariety;
    }

    void TcBasReportManager::select_chemical_from_catalog(const SorProduct &product)
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        auto &s = currentTask.sorData;

        s.chemicalName = product.name;
        s.permitNumber = product.permit;
        if (product.defaultDose > 0.0f) s.dosePerHa = product.defaultDose;
        s.doseUnit = product.defaultDoseUnit;

        // Sugestia agrofaga / przeznaczenia na podstawie rodzaju i substancji
        if (product.kind.find("Chwast") != std::string::npos)
            s.targetPest = "Chwasty jedno- i dwuliścienne (subst: " + product.substance + ")";
        else if (product.kind.find("Fungicyd") != std::string::npos)
            s.targetPest = "Choroby grzybowe (subst: " + product.substance + ")";
        else if (product.kind.find("Insektycyd") != std::string::npos)
            s.targetPest = "Szkodniki upraw (subst: " + product.substance + ")";
        else if (product.kind.find("Regulator") != std::string::npos)
            s.targetPest = "Regulacja pokroju rośliny i zapobieganie wyleganiu";

        currentTask.appliedProductTotal = currentTask.workedAreaHa * s.dosePerHa;
        s.totalChemicalUsed = currentTask.appliedProductTotal;
    }

    std::string TcBasReportManager::get_current_iso_time()
    {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm;
        localtime_s(&tm, &t);

        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
        return oss.str();
    }

    std::string TcBasReportManager::format_time_hhmmss(float seconds)
    {
        int totalSec = static_cast<int>(seconds);
        int h = totalSec / 3600;
        int m = (totalSec % 3600) / 60;
        int s = totalSec % 60;

        char buf[32];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
        return std::string(buf);
    }

    void TcBasReportManager::start_task(const std::string &taskName, const std::string &fieldName, float widthM)
    {
        std::lock_guard<std::mutex> lock(dataMutex);

        currentTask.taskId = "TSK" + std::to_string(taskHistory.size() + 1);
        if (!taskName.empty()) currentTask.taskName = taskName;
        if (!fieldName.empty()) currentTask.fieldName = fieldName;
        if (widthM > 0.0f) currentTask.implementWidthMeters = widthM;

        currentTask.startTime = std::chrono::system_clock::now();
        currentTask.elapsedWorkSeconds = 0.0f;
        currentTask.workedAreaHa = 0.0f;
        currentTask.workedDistanceKm = 0.0f;
        currentTask.appliedProductTotal = 0.0f;
        currentTask.isRunning = true;
        currentTask.isPaused = false;

        currentTask.sorData.recordId = currentTask.taskId;
        currentTask.sorData.startTimeIso = get_current_iso_time();
        currentTask.sorData.endTimeIso = "";
        currentTask.sorData.appliedAreaHa = 0.0f;
        currentTask.sorData.totalChemicalUsed = 0.0f;
        currentTask.sorData.isCompleted = false;
    }

    void TcBasReportManager::pause_task()
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        if (currentTask.isRunning)
        {
            currentTask.isPaused = true;
        }
    }

    void TcBasReportManager::resume_task()
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        if (currentTask.isRunning)
        {
            currentTask.isPaused = false;
        }
    }

    void TcBasReportManager::stop_task()
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        if (!currentTask.isRunning) return;

        currentTask.stopTime = std::chrono::system_clock::now();
        currentTask.isRunning = false;
        currentTask.isPaused = false;

        currentTask.sorData.endTimeIso = get_current_iso_time();
        currentTask.sorData.appliedAreaHa = currentTask.workedAreaHa;
        currentTask.sorData.totalChemicalUsed = currentTask.appliedProductTotal;
        currentTask.sorData.isCompleted = true;

        taskHistory.push_back(currentTask);
        save_history_to_disk();
    }

    bool TcBasReportManager::is_task_active() const
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        return currentTask.isRunning && !currentTask.isPaused;
    }

    bool TcBasReportManager::is_task_paused() const
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        return currentTask.isRunning && currentTask.isPaused;
    }

    bool TcBasReportManager::scan_and_sync_agopengps()
    {
        std::lock_guard<std::mutex> lock(dataMutex);

        char userProfile[MAX_PATH] = { 0 };
        GetEnvironmentVariableA("USERPROFILE", userProfile, MAX_PATH);
        std::string aogBase = std::string(userProfile) + "\\Documents\\AgOpenGPS";
        if (!std::filesystem::exists(aogBase))
            return false;

        std::string aogFieldName = "";
        std::string aogToolName = "";
        float aogToolWidth = 0.0f;
        int aogSections = 0;
        double aogLat = 0.0, aogLon = 0.0;

        // 1. Sprawdź Environment/environment.xml dla aktywnego pola
        std::string envXml = aogBase + "\\Environment\\environment.xml";
        if (std::filesystem::exists(envXml))
        {
            std::ifstream ef(envXml);
            if (ef.is_open())
            {
                std::stringstream ss;
                ss << ef.rdbuf();
                std::string xmlStr = ss.str();
                std::string key = "<setting name=\"setF_CurrentDir\" serializeAs=\"String\">";
                size_t p = xmlStr.find(key);
                if (p != std::string::npos)
                {
                    size_t vStart = xmlStr.find("<value>", p);
                    size_t vEnd = xmlStr.find("</value>", vStart);
                    if (vStart != std::string::npos && vEnd != std::string::npos)
                    {
                        aogFieldName = xmlStr.substr(vStart + 7, vEnd - (vStart + 7));
                    }
                }
            }
        }

        // 2. Znajdź najnowszy/najbardziej pasujący profil narzędzia w ToolProfiles i Vehicles
        std::vector<std::string> searchDirs = {
            aogBase + "\\Vehicles",
            aogBase + "\\ToolProfiles",
            aogBase + "\\VehicleProfiles"
        };

        std::filesystem::file_time_type newestTime{};
        std::filesystem::path newestToolPath;

        for (const auto &sDir : searchDirs)
        {
            if (!std::filesystem::exists(sDir)) continue;
            for (const auto &entry : std::filesystem::directory_iterator(sDir))
            {
                if (entry.is_regular_file())
                {
                    std::string ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".xml")
                    {
                        auto ft = entry.last_write_time();
                        // Preferuj pliki z nazwą tecnoma lub zawierające szerokość, lub najnowszy plik
                        std::string stemLower = entry.path().stem().string();
                        std::transform(stemLower.begin(), stemLower.end(), stemLower.begin(), ::tolower);
                        bool isPreferred = (stemLower.find("tecnoma") != std::string::npos || stemLower.find("28m") != std::string::npos);

                        if (newestToolPath.empty() || isPreferred || ft > newestTime)
                        {
                            newestTime = ft;
                            newestToolPath = entry.path();
                            if (isPreferred && sDir.find("Vehicles") != std::string::npos)
                            {
                                // Dokładny profil maszyny z AgOpenGPS Vehicles (np. tecnoma28m.XML)
                                break;
                            }
                        }
                    }
                }
            }
        }

        if (!newestToolPath.empty())
        {
            aogToolName = newestToolPath.stem().string();
            std::ifstream tf(newestToolPath);
            if (tf.is_open())
            {
                std::stringstream ss;
                ss << tf.rdbuf();
                std::string tXml = ss.str();

                auto readSetting = [&](const std::string &settingName) -> std::string {
                    std::string k = "name=\"" + settingName + "\"";
                    size_t pos = tXml.find(k);
                    if (pos == std::string::npos) return "";
                    size_t vs = tXml.find("<value>", pos);
                    size_t ve = tXml.find("</value>", vs);
                    if (vs == std::string::npos || ve == std::string::npos) return "";
                    return tXml.substr(vs + 7, ve - (vs + 7));
                };

                std::string wStr = readSetting("setVehicle_toolWidth");
                if (!wStr.empty())
                {
                    try { aogToolWidth = std::stof(wStr); } catch (...) {}
                }
                std::string sStr = readSetting("setVehicle_numSections");
                if (!sStr.empty())
                {
                    try { aogSections = std::stoi(sStr); } catch (...) {}
                }
            }
        }

        // 3. Sprawdź koordynaty i pliki w Fields/<fieldName>
        if (!aogFieldName.empty())
        {
            std::string fieldPath = aogBase + "\\Fields\\" + aogFieldName + "\\Field.txt";
            if (std::filesystem::exists(fieldPath))
            {
                std::ifstream ff(fieldPath);
                std::string line;
                bool nextIsFix = false;
                while (std::getline(ff, line))
                {
                    if (line.find("StartFix") != std::string::npos)
                    {
                        nextIsFix = true;
                        continue;
                    }
                    if (nextIsFix)
                    {
                        size_t comma = line.find(',');
                        if (comma != std::string::npos)
                        {
                            try {
                                aogLat = std::stod(line.substr(0, comma));
                                aogLon = std::stod(line.substr(comma + 1));
                            } catch (...) {}
                        }
                        break;
                    }
                }
            }
        }

        // 4. Zaaplikuj dane do aktualnego zadania TC-BAS i ewidencji
        if (!aogFieldName.empty())
        {
            currentTask.fieldName = aogFieldName;
            currentTask.sorData.fieldName = aogFieldName;
        }

        if (aogToolWidth > 0.0f)
        {
            currentTask.implementWidthMeters = aogToolWidth;
            if (aogSections > 0)
            {
                currentTask.implementSectionsCount = aogSections;
            }
            if (!aogToolName.empty())
            {
                currentTask.implementName = aogToolName + " (" + std::to_string(static_cast<int>(aogToolWidth)) + "m AgOpenGPS)";
                currentTask.sorData.sprayerModel = currentTask.implementName;
            }
        }

        // Dopasuj działkę ewidencyjną z profilu gospodarstwa jeśli pasuje
        for (const auto &p : farmProfile.parcels)
        {
            if (p.name == aogFieldName || aogFieldName.find(p.name) != std::string::npos || p.name.find(aogFieldName) != std::string::npos)
            {
                currentTask.sorData.cadastralParcel = p.parcelNumber;
                currentTask.sorData.cadastralPrecinct = p.precinct;
                currentTask.sorData.commune = p.commune;
                currentTask.sorData.totalFieldAreaHa = p.areaHa;
                if (!p.defaultCrop.empty()) currentTask.sorData.cropName = p.defaultCrop;
                if (!p.defaultVariety.empty()) currentTask.sorData.cropVariety = p.defaultVariety;
                break;
            }
        }

        std::ostringstream oss;
        oss << "Połączono z AgOpenGPS | Pole: '" << (aogFieldName.empty() ? "Domyślne" : aogFieldName) << "'";
        if (aogToolWidth > 0.0f) oss << " | Belka: " << aogToolWidth << " m (" << (aogSections > 0 ? std::to_string(aogSections) : "6") << " sekcji)";
        if (aogLat != 0.0) oss << " | GPS: " << std::fixed << std::setprecision(5) << aogLat << "N, " << aogLon << "E";
        lastAogSyncInfo = oss.str();

        return true;
    }

    void TcBasReportManager::set_field_name_from_aog(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        if (name.empty()) return;

        currentTask.fieldName = name;
        currentTask.sorData.fieldName = name;

        // Auto-start zadania jeśli nie działa
        if (!currentTask.isRunning)
        {
            currentTask.isRunning = true;
            currentTask.isPaused = false;
            currentTask.startTime = std::chrono::system_clock::now();
            currentTask.sorData.startTimeIso = get_current_iso_time();
        }

        // Dopasuj działkę
        for (const auto &p : farmProfile.parcels)
        {
            if (p.name == name || name.find(p.name) != std::string::npos || p.name.find(name) != std::string::npos)
            {
                currentTask.sorData.cadastralParcel = p.parcelNumber;
                currentTask.sorData.cadastralPrecinct = p.precinct;
                currentTask.sorData.commune = p.commune;
                currentTask.sorData.totalFieldAreaHa = p.areaHa;
                if (!p.defaultCrop.empty()) currentTask.sorData.cropName = p.defaultCrop;
                if (!p.defaultVariety.empty()) currentTask.sorData.cropVariety = p.defaultVariety;
                break;
            }
        }

        lastAogSyncInfo = "Aktywne pole AgOpenGPS: '" + name + "'";
    }

    void TcBasReportManager::update_work_data(float speedKph, float activeSectionRatio, float dtSeconds)
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        if (dtSeconds <= 0.0f || dtSeconds > 5.0f) return;

        // Auto-start zadania w tle, jeśli maszyna pracuje (sekcje aktywne lub prędkość > 0.1 km/h)
        if (!currentTask.isRunning && (activeSectionRatio > 0.0f || speedKph >= 0.2f))
        {
            currentTask.isRunning = true;
            currentTask.isPaused = false;
            currentTask.startTime = std::chrono::system_clock::now();
            currentTask.sorData.startTimeIso = get_current_iso_time();
        }

        if (!currentTask.isRunning || currentTask.isPaused) return;

        // Akumuluj czas pracy i dane, gdy sekcje są otwarte
        if (activeSectionRatio > 0.0f)
        {
            currentTask.elapsedWorkSeconds += dtSeconds;

            if (speedKph >= 0.1f)
            {
                float activeWidth = currentTask.implementWidthMeters * std::clamp(activeSectionRatio, 0.0f, 1.0f);
                float speedMs = speedKph / 3.6f;
                float distanceM = speedMs * dtSeconds;
                float areaM2 = activeWidth * distanceM;

                currentTask.workedAreaHa += (areaM2 / 10000.0f);
                currentTask.workedDistanceKm += (distanceM / 1000.0f);

                // Aktualizacja dawki preparatu i ewidencji
                currentTask.appliedProductTotal = currentTask.workedAreaHa * currentTask.sorData.dosePerHa;
                currentTask.sorData.appliedAreaHa = currentTask.workedAreaHa;
                currentTask.sorData.totalChemicalUsed = currentTask.appliedProductTotal;

                if (currentTask.avgSpeedKph <= 0.1f)
                    currentTask.avgSpeedKph = speedKph;
                else
                    currentTask.avgSpeedKph = (currentTask.avgSpeedKph * 0.98f) + (speedKph * 0.02f);
            }
        }
    }

    void TcBasReportManager::clear_history()
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        taskHistory.clear();
        save_history_to_disk();
    }

    void TcBasReportManager::set_predefined_sor(int templateIndex)
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        auto &s = currentTask.sorData;

        switch (templateIndex)
        {
        case 0: // Herbicyd zbóż
            s.chemicalName = "Boxer 800 EC";
            s.permitNumber = "R-88/2015";
            s.cropName = "Pszenica ozima";
            s.bbchStage = "BBCH 12-14 (faza 2-4 liści)";
            s.dosePerHa = 3.0f;
            s.doseUnit = "l/ha";
            s.waterPerHa = 200.0f;
            s.targetPest = "Miotła zbożowa, przytulia czepna";
            s.temperatureC = 14.5f;
            s.windSpeedMs = 2.0f;
            s.weatherConditions = "Optymalne, bezwietrznie";
            break;

        case 1: // Fungicyd T1 zbóż
            s.chemicalName = "Delaro 325 SC";
            s.permitNumber = "R-18/2016wu";
            s.cropName = "Pszenica ozima";
            s.bbchStage = "BBCH 31-32 (strzelanie w źdźbło, 1-2 kolanko)";
            s.dosePerHa = 1.0f;
            s.doseUnit = "l/ha";
            s.waterPerHa = 200.0f;
            s.targetPest = "Mączniak prawdziwy zbóż, septorioza paskowana, łamliwość źdźbła";
            s.temperatureC = 17.0f;
            s.windSpeedMs = 1.8f;
            s.weatherConditions = "Ciepło, wilgotno po porannej rosie";
            break;

        case 2: // Fungicyd T2 liść flagowy
            s.chemicalName = "Elatus Era";
            s.permitNumber = "R-229/2017";
            s.cropName = "Pszenżyto ozime";
            s.bbchStage = "BBCH 39 (w pełni rozwinięty liść flagowy)";
            s.dosePerHa = 0.8f;
            s.doseUnit = "l/ha";
            s.waterPerHa = 220.0f;
            s.targetPest = "Rdza brunatna, septorioza liści, rynchosporioza";
            s.temperatureC = 19.5f;
            s.windSpeedMs = 2.5f;
            s.weatherConditions = "Umiarkowane zachmurzenie";
            break;

        case 3: // Insektycyd
            s.chemicalName = "Karate Zeon 050 CS";
            s.permitNumber = "R-15/2015";
            s.cropName = "Rzepak ozimy";
            s.bbchStage = "BBCH 51-55 (pąkowanie / zielony pąk)";
            s.dosePerHa = 0.12f;
            s.doseUnit = "l/ha";
            s.waterPerHa = 250.0f;
            s.targetPest = "Słodyszek rzepakowy, chowacz czterozębny";
            s.temperatureC = 15.0f;
            s.windSpeedMs = 1.5f;
            s.weatherConditions = "Słonecznie, po oblocie pszczół wieczorem";
            break;

        case 4: // Kukurydza odchwaszczanie
            s.chemicalName = "Lumax Duo";
            s.permitNumber = "R-128/2024";
            s.cropName = "Kukurydza";
            s.bbchStage = "BBCH 13-15 (faza 3-5 liści kukurydzy)";
            s.dosePerHa = 3.5f;
            s.doseUnit = "l/ha";
            s.waterPerHa = 250.0f;
            s.targetPest = "Chwastnica jednostronna, komosa biała, rdesty";
            s.temperatureC = 18.0f;
            s.windSpeedMs = 2.2f;
            s.weatherConditions = "Wilgotna gleba po opadach";
            break;

        case 5: // Rzepak skracanie / grzyb
            s.chemicalName = "Caryx 240 SL";
            s.permitNumber = "R-71/2017";
            s.cropName = "Rzepak ozimy";
            s.bbchStage = "BBCH 14-16 (faza 4-6 liści jesienią)";
            s.dosePerHa = 1.0f;
            s.doseUnit = "l/ha";
            s.waterPerHa = 200.0f;
            s.targetPest = "Sucha zgnilizna kapustnych, regulacja pokroju rośliny";
            s.temperatureC = 13.0f;
            s.windSpeedMs = 2.0f;
            s.weatherConditions = "Jesienna aura, chłodno";
            break;
        }

        currentTask.appliedProductTotal = currentTask.workedAreaHa * s.dosePerHa;
        s.totalChemicalUsed = currentTask.appliedProductTotal;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Eksport oficjalnego ISO-XML (ISO 11783-10: TASKDATA.XML)
    // ─────────────────────────────────────────────────────────────────────────────
    bool TcBasReportManager::export_taskdata_xml(const std::string &outputDirectory, std::string *outFilePath)
    {
        std::lock_guard<std::mutex> lock(dataMutex);

        try
        {
            std::filesystem::create_directories(outputDirectory);
        }
        catch (...)
        {
            return false;
        }

        std::string fullPath = outputDirectory + "/TASKDATA.XML";
        std::ofstream xml(fullPath, std::ios::trunc);
        if (!xml.is_open()) return false;

        const auto &t = currentTask;
        std::string nowIso = get_current_iso_time();
        std::string startIso = t.sorData.startTimeIso.empty() ? nowIso : t.sorData.startTimeIso;
        std::string stopIso = t.sorData.endTimeIso.empty() ? nowIso : t.sorData.endTimeIso;

        float areaM2 = t.workedAreaHa * 10000.0f;
        float distM = t.workedDistanceKm * 1000.0f;
        int durationSec = static_cast<int>(t.elapsedWorkSeconds);
        float totalChemicalLiters = t.appliedProductTotal;

        xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        xml << "<ISO11783_TaskData VersionMajor=\"4\" VersionMinor=\"2\" "
            << "ManagementSoftwareManufacturer=\"AG-ISOBUS PRO\" ManagementSoftwareVersion=\"2.5\" "
            << "TaskControllerManufacturer=\"AG-ISOBUS PRO TC-BAS\" TaskControllerVersion=\"2.5\" "
            << "DataTransferOrigin=\"1\">\n";

        // CTR (Customer)
        xml << "  <CTR A=\"CTR1\" B=\"" << t.customerName << "\" />\n";

        // FRM (Farm)
        xml << "  <FRM A=\"FRM1\" B=\"" << t.farmName << "\" I=\"CTR1\" />\n";

        // PFD (Partfield)
        xml << "  <PFD A=\"PFD1\" B=\"" << t.fieldName << " [Dz. " << t.sorData.cadastralParcel << "]\" "
            << "C=\"" << static_cast<int>(t.sorData.totalFieldAreaHa * 10000.0f) << "\" D=\"FRM1\" />\n";

        // WKR (Worker)
        xml << "  <WKR A=\"WKR1\" B=\"" << t.sorData.operatorName << "\" C=\"Operator ŚOR [" << t.sorData.operatorCertificateNo << "]\" />\n";

        // DVC (Device - Implement)
        xml << "  <DVC A=\"DVC1\" B=\"" << t.implementName << " [" << t.sorData.sprayerInspectionNo << "]\" "
            << "C=\"" << static_cast<int>(t.implementWidthMeters * 1000.0f) << "\" D=\"Opryskiwacz ISOBUS\" />\n";

        // PDT (Product - Środek ochrony roślin)
        xml << "  <PDT A=\"PDT1\" B=\"" << t.sorData.chemicalName << " (Zezw: " << t.sorData.permitNumber << ")\" C=\"1\" />\n";

        // TSK (Task)
        std::string taskStatus = t.isRunning ? (t.isPaused ? "3" : "2") : "4";
        xml << "  <TSK A=\"" << t.taskId << "\" B=\"" << t.taskName << " - " << t.sorData.cropName << "\" "
            << "C=\"CTR1\" D=\"FRM1\" E=\"PFD1\" G=\"" << taskStatus << "\">\n";

        // TIM (Time Log)
        xml << "    <TIM A=\"4\" D=\"" << startIso << "\" E=\"" << stopIso << "\" G=\"" << durationSec << "\" />\n";

        // DAN (Data Allocation Network: Total counters: Area, Time, Distance)
        xml << "    <DAN A=\"0001\" B=\"TotalArea\" C=\"" << static_cast<int>(areaM2) << "\" />\n";
        xml << "    <DAN A=\"0002\" B=\"EffectiveTime\" C=\"" << durationSec << "\" />\n";
        xml << "    <DAN A=\"0003\" B=\"TotalDistance\" C=\"" << static_cast<int>(distM) << "\" />\n";

        // PAN (Product Allocation Network: Dawka i łączne zużycie)
        xml << "    <PAN A=\"PDT1\">\n";
        xml << "      <PRN A=\"DosePerHa\" B=\"" << std::fixed << std::setprecision(2) << t.sorData.dosePerHa << "\" />\n";
        xml << "      <PRN A=\"TotalChemicalLiters\" B=\"" << std::fixed << std::setprecision(2) << totalChemicalLiters << "\" />\n";
        xml << "      <PRN A=\"WaterVolumePerHa\" B=\"" << std::fixed << std::setprecision(1) << t.sorData.waterPerHa << "\" />\n";
        xml << "    </PAN>\n";

        xml << "  </TSK>\n";
        xml << "</ISO11783_TaskData>\n";

        xml.close();
        if (outFilePath) *outFilePath = fullPath;
        lastExportedXmlPath = fullPath;
        return true;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Eksport oficjalnej Ewidencji Zabiegów ŚOR (HTML gotowy do druku A4 dla PIORiN)
    // ─────────────────────────────────────────────────────────────────────────────
    bool TcBasReportManager::export_sor_html_report(const SorRecord &r, const std::string &outputDirectory, std::string *outFilePath)
    {
        try
        {
            std::filesystem::create_directories(outputDirectory);
        }
        catch (...)
        {
            return false;
        }

        std::string filename = "Ewidencja_SOR_" + r.recordId + ".html";
        std::string fullPath = outputDirectory + "/" + filename;
        std::ofstream h(fullPath, std::ios::trunc);
        if (!h.is_open()) return false;

        std::string nowIso = get_current_iso_time();
        std::string startIso = r.startTimeIso.empty() ? nowIso : r.startTimeIso;
        std::string endIso = r.endTimeIso.empty() ? nowIso : r.endTimeIso;

        h << "<!DOCTYPE html>\n<html lang=\"pl\">\n<head>\n";
        h << "<meta charset=\"UTF-8\">\n";
        h << "<title>Urzędowa Ewidencja Zabiegów ŚOR - " << r.fieldName << "</title>\n";
        h << "<style>\n";
        h << "  @page { size: A4 portrait; margin: 12mm 15mm 15mm 15mm; }\n";
        h << "  body { font-family: 'Segoe UI', Tahoma, Arial, sans-serif; font-size: 11pt; color: #1a1a1a; margin: 0; padding: 20px; background: #fdfdfd; }\n";
        h << "  .container { max-width: 820px; margin: auto; background: white; padding: 25px; border: 1px solid #ddd; box-shadow: 0 4px 12px rgba(0,0,0,0.06); }\n";
        h << "  .header { text-align: center; border-bottom: 2px solid #2e7d32; padding-bottom: 12px; margin-bottom: 20px; }\n";
        h << "  .header h1 { font-size: 15pt; color: #1b5e20; margin: 0 0 6px 0; text-transform: uppercase; letter-spacing: 0.5px; }\n";
        h << "  .header h2 { font-size: 10pt; color: #555; font-weight: normal; margin: 0; }\n";
        h << "  .badge { display: inline-block; background: #e8f5e9; color: #2e7d32; padding: 4px 10px; border-radius: 4px; font-weight: bold; font-size: 9pt; margin-top: 8px; border: 1px solid #c8e6c9; }\n";
        h << "  .section-title { font-size: 11pt; font-weight: bold; color: #1b5e20; margin: 18px 0 8px 0; border-left: 4px solid #2e7d32; padding-left: 8px; background: #f1f8e9; padding-top: 4px; padding-bottom: 4px; }\n";
        h << "  table.data-table { width: 100%; border-collapse: collapse; margin-bottom: 14px; font-size: 9.5pt; }\n";
        h << "  table.data-table th, table.data-table td { border: 1px solid #b0bec5; padding: 6px 8px; text-align: left; vertical-align: top; }\n";
        h << "  table.data-table th { background: #eceff1; color: #263238; font-weight: 600; width: 35%; }\n";
        h << "  table.data-table td.val-highlight { font-weight: bold; color: #0d47a1; background: #fbfcfe; }\n";
        h << "  .summary-box { background: #e8f5e9; border: 1px solid #a5d6a7; border-radius: 6px; padding: 12px; margin: 15px 0; }\n";
        h << "  .summary-box h3 { margin: 0 0 8px 0; color: #1b5e20; font-size: 11pt; }\n";
        h << "  .summary-grid { display: flex; justify-content: space-between; font-size: 10pt; }\n";
        h << "  .signatures { display: flex; justify-content: space-between; margin-top: 40px; padding-top: 10px; }\n";
        h << "  .sig-box { width: 45%; text-align: center; border-top: 1px dashed #78909c; padding-top: 6px; font-size: 9pt; color: #455a64; }\n";
        h << "  .footer { margin-top: 25px; font-size: 8pt; color: #90a4ae; text-align: center; border-top: 1px solid #eceff1; padding-top: 8px; }\n";
        h << "  .no-print { text-align: right; margin-bottom: 15px; }\n";
        h << "  .btn-print { background: #2e7d32; color: white; border: none; padding: 8px 16px; font-size: 10pt; font-weight: bold; border-radius: 4px; cursor: pointer; }\n";
        h << "  .btn-print:hover { background: #1b5e20; }\n";
        h << "  @media print { .no-print { display: none; } body { padding: 0; background: white; } .container { border: none; box-shadow: none; padding: 0; } }\n";
        h << "</style>\n</head>\n<body>\n";

        h << "<div class=\"container\">\n";
        h << "  <div class=\"no-print\"><button class=\"btn-print\" onclick=\"window.print()\">🖨️ Drukuj Kartę Ewidencji (A4)</button></div>\n";

        h << "  <div class=\"header\">\n";
        h << "    <h1>Ewidencja Zabiegów Środkami Ochrony Roślin</h1>\n";
        h << "    <h2>Wymóg prawny: Rozporządzenie Parlamentu Europejskiego i Rady (WE) nr 1107/2009 (art. 67) & Przepisy Krajowe 2026/2027</h2>\n";
        h << "    <div class=\"badge\">STATUS: DOKUMENT URZĘDOWY - ZAPIS TC-BAS / GPS</div>\n";
        h << "  </div>\n";

        // Podmiot i działka
        h << "  <div class=\"section-title\">1. DANE GOSPODARSTWA I IDENTYFIKACJA DZIAŁKI</div>\n";
        h << "  <table class=\"data-table\">\n";
        h << "    <tr><th>Gospodarstwo / Podmiot:</th><td>" << r.farmName << "</td></tr>\n";
        h << "    <tr><th>Nazwa pola / Działki rolnej:</th><td class=\"val-highlight\">" << r.fieldName << "</td></tr>\n";
        h << "    <tr><th>Nr działki ewidencyjnej / Obręb:</th><td>Dz. ew. <b>" << r.cadastralParcel << "</b>, Obręb: " << r.cadastralPrecinct << "</td></tr>\n";
        h << "    <tr><th>Gmina / Powiat:</th><td>" << r.commune << "</td></tr>\n";
        h << "    <tr><th>Powierzchnia całkowita działki:</th><td>" << std::fixed << std::setprecision(2) << r.totalFieldAreaHa << " ha</td></tr>\n";
        h << "  </table>\n";

        // Roślina i faza
        h << "  <div class=\"section-title\">2. ROŚLINA UPRAWNA I PRZYCZYNA ZABIEGU (AGROFAG)</div>\n";
        h << "  <table class=\"data-table\">\n";
        h << "    <tr><th>Gatunek uprawy / Odmiana:</th><td class=\"val-highlight\">" << r.cropName << " (odmiana: " << r.cropVariety << ")</td></tr>\n";
        h << "    <tr><th>Faza rozwojowa (skala BBCH):</th><td><b>" << r.bbchStage << "</b></td></tr>\n";
        h << "    <tr><th>Przyczyna zabiegu / Zwalczany agrofag:</th><td>" << r.targetPest << "</td></tr>\n";
        h << "  </table>\n";

        // Preparat
        h << "  <div class=\"section-title\">3. ZASTOSOWANY ŚRODEK OCHRONY ROŚLIN (ŚOR)</div>\n";
        h << "  <table class=\"data-table\">\n";
        h << "    <tr><th>Nazwa handlowa preparatu:</th><td class=\"val-highlight\" style=\"font-size:11pt;\">" << r.chemicalName << "</td></tr>\n";
        h << "    <tr><th>Numer zezwolenia MRiRW:</th><td>" << r.permitNumber << "</td></tr>\n";
        h << "    <tr><th>Zastosowana dawka środka:</th><td class=\"val-highlight\">" << std::fixed << std::setprecision(2) << r.dosePerHa << " " << r.doseUnit << "</td></tr>\n";
        h << "    <tr><th>Ilość cieczy użytkowej / wody:</th><td>" << std::fixed << std::setprecision(1) << r.waterPerHa << " l/ha</td></tr>\n";
        h << "  </table>\n";

        // Podsumowanie wykonania zabiegu
        h << "  <div class=\"summary-box\">\n";
        h << "    <h3>4. BILANS WYKONANEGO ZABIEGU (ZLICZENIE GPS / TASK CONTROLLER)</h3>\n";
        h << "    <div class=\"summary-grid\">\n";
        h << "      <div><b>Data rozpoczęcia:</b><br>" << startIso << "</div>\n";
        h << "      <div><b>Data zakończenia:</b><br>" << endIso << "</div>\n";
        h << "      <div><b>Powierzchnia opryskana:</b><br><span style=\"color:#2e7d32; font-size:12pt; font-weight:bold;\">"
          << std::fixed << std::setprecision(2) << r.appliedAreaHa << " ha</span></div>\n";
        h << "      <div><b>Łączne zużycie środka:</b><br><span style=\"color:#c62828; font-size:12pt; font-weight:bold;\">"
          << std::fixed << std::setprecision(2) << r.totalChemicalUsed << " " << r.doseUnit.substr(0, r.doseUnit.find('/')) << "</span></div>\n";
        h << "    </div>\n";
        h << "  </div>\n";

        // Warunki pogodowe, sprzęt i operator
        h << "  <div class=\"section-title\">5. WARUNKI ATMOSFERYCZNE, SPRZĘT I OSOBA WYKONUJĄCA</div>\n";
        h << "  <table class=\"data-table\">\n";
        h << "    <tr><th>Warunki pogodowe (temp, wiatr, wilgotność):</th><td>"
          << "Temp: <b>" << std::fixed << std::setprecision(1) << r.temperatureC << " °C</b> | "
          << "Wiatr: <b>" << std::fixed << std::setprecision(1) << r.windSpeedMs << " m/s</b> (&lt; 4.0 m/s dopuszczalna) | "
          << "Wilgotność: <b>" << std::fixed << std::setprecision(0) << r.humidityPct << "%</b><br>"
          << "<small style=\"color:#555;\">" << r.weatherConditions << "</small></td></tr>\n";
        h << "    <tr><th>Sprzęt / Opryskiwacz:</th><td>" << r.sprayerModel << "</td></tr>\n";
        h << "    <tr><th>Atest techniczny opryskiwacza:</th><td>Nr badania: <b>" << r.sprayerInspectionNo << "</b> (ważny do: " << r.sprayerInspectionValidUntil << ")</td></tr>\n";
        h << "    <tr><th>Osoba wykonująca zabieg:</th><td><b>" << r.operatorName << "</b></td></tr>\n";
        h << "    <tr><th>Zaświadczenie o ukończeniu szkolenia ŚOR:</th><td>Nr certyfikatu: <b>" << r.operatorCertificateNo << "</b></td></tr>\n";
        h << "  </table>\n";

        // Podpisy
        h << "  <div class=\"signatures\">\n";
        h << "    <div class=\"sig-box\"><br><br>...........................................................................<br>Podpis osoby wykonującej zabieg</div>\n";
        h << "    <div class=\"sig-box\"><br><br>...........................................................................<br>Podpis kierownika gospodarstwa / posiadacza gruntów</div>\n";
        h << "  </div>\n";

        h << "  <div class=\"footer\">\n";
        h << "    AG-ISOBUS PRO Universal Terminal & TC-BAS | Raport wygenerowano automatycznie z danych magistrali CAN / GPS: " << nowIso << "\n";
        h << "  </div>\n";

        h << "</div>\n</body>\n</html>\n";
        h.close();

        if (outFilePath) *outFilePath = fullPath;
        lastExportedHtmlPath = fullPath;
        return true;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // Eksport zbiorczego rejestru CSV (do Excela / systemów e-ŚOR)
    // ─────────────────────────────────────────────────────────────────────────────
    bool TcBasReportManager::export_sor_csv_register(const std::string &outputDirectory, std::string *outFilePath)
    {
        std::lock_guard<std::mutex> lock(dataMutex);

        try
        {
            std::filesystem::create_directories(outputDirectory);
        }
        catch (...)
        {
            return false;
        }

        std::string fullPath = outputDirectory + "/Rejestr_Zabiegow_SOR_2027.csv";
        std::ofstream csv(fullPath, std::ios::trunc);
        if (!csv.is_open()) return false;

        csv << "\xEF\xBB\xBF";

        csv << "Lp.;ID Zadania;Data zabiegu;Gospodarstwo;Nazwa pola;Nr dzialki;Obreb;Gmina;Powierzchnia dzialki [ha];"
            << "Powierzchnia zabiegu [ha];Uprawa;Faza BBCH;Srodek ochrony roslin;Nr zezwolenia MRiRW;"
            << "Dawka srodka;Jednostka dawki;Ciecz uzytkowa [l/ha];Laczne zuzycie srodka;Zwalczany agrofag;"
            << "Temperatura [C];Predkosc wiatru [m/s];Opryskiwacz;Atest opryskiwacza;Waznosc atestu;"
            << "Osoba wykonujaca;Nr zaswiadczenia SOR\n";

        auto writeRow = [&](int index, const SorRecord &r) {
            csv << index << ";"
                << r.recordId << ";"
                << r.startTimeIso << ";"
                << "\"" << r.farmName << "\";"
                << "\"" << r.fieldName << "\";"
                << "\"" << r.cadastralParcel << "\";"
                << "\"" << r.cadastralPrecinct << "\";"
                << "\"" << r.commune << "\";"
                << std::fixed << std::setprecision(2) << r.totalFieldAreaHa << ";"
                << std::fixed << std::setprecision(2) << r.appliedAreaHa << ";"
                << "\"" << r.cropName << "\";"
                << "\"" << r.bbchStage << "\";"
                << "\"" << r.chemicalName << "\";"
                << "\"" << r.permitNumber << "\";"
                << std::fixed << std::setprecision(2) << r.dosePerHa << ";"
                << "\"" << r.doseUnit << "\";"
                << std::fixed << std::setprecision(1) << r.waterPerHa << ";"
                << std::fixed << std::setprecision(2) << r.totalChemicalUsed << ";"
                << "\"" << r.targetPest << "\";"
                << std::fixed << std::setprecision(1) << r.temperatureC << ";"
                << std::fixed << std::setprecision(1) << r.windSpeedMs << ";"
                << "\"" << r.sprayerModel << "\";"
                << "\"" << r.sprayerInspectionNo << "\";"
                << "\"" << r.sprayerInspectionValidUntil << "\";"
                << "\"" << r.operatorName << "\";"
                << "\"" << r.operatorCertificateNo << "\"\n";
        };

        int idx = 1;
        for (const auto &hist : taskHistory)
        {
            writeRow(idx++, hist.sorData);
        }

        if (currentTask.workedAreaHa > 0.0f)
        {
            writeRow(idx++, currentTask.sorData);
        }

        csv.close();
        if (outFilePath) *outFilePath = fullPath;
        return true;
    }

    void TcBasReportManager::load_history_from_disk()
    {
        std::string p = "Raporty_SOR/Rejestr_Zabiegow_SOR_2027.csv";
        if (!std::filesystem::exists(p)) return;

        std::ifstream file(p);
        if (!file.is_open()) return;

        std::string line;
        std::getline(file, line);

        while (std::getline(file, line))
        {
            if (line.empty()) continue;
            std::stringstream ss(line);
            std::string token;
            std::vector<std::string> cols;

            while (std::getline(ss, token, ';'))
            {
                if (token.size() >= 2 && token.front() == '"' && token.back() == '"')
                    token = token.substr(1, token.size() - 2);
                cols.push_back(token);
            }

            if (cols.size() >= 25)
            {
                TcTaskSession s;
                s.taskId = cols[1];
                s.taskName = "Zabieg " + cols[12];
                s.sorData.recordId = cols[1];
                s.sorData.startTimeIso = cols[2];
                s.sorData.farmName = cols[3];
                s.sorData.fieldName = cols[4];
                s.sorData.cadastralParcel = cols[5];
                s.sorData.cadastralPrecinct = cols[6];
                s.sorData.commune = cols[7];
                try {
                    s.sorData.totalFieldAreaHa = std::stof(cols[8]);
                    s.sorData.appliedAreaHa = std::stof(cols[9]);
                    s.workedAreaHa = s.sorData.appliedAreaHa;
                } catch (...) {}
                s.sorData.cropName = cols[10];
                s.sorData.bbchStage = cols[11];
                s.sorData.chemicalName = cols[12];
                s.sorData.permitNumber = cols[13];
                try {
                    s.sorData.dosePerHa = std::stof(cols[14]);
                } catch (...) {}
                s.sorData.doseUnit = cols[15];
                try {
                    s.sorData.waterPerHa = std::stof(cols[16]);
                    s.sorData.totalChemicalUsed = std::stof(cols[17]);
                    s.appliedProductTotal = s.sorData.totalChemicalUsed;
                } catch (...) {}
                s.sorData.targetPest = cols[18];
                try {
                    s.sorData.temperatureC = std::stof(cols[19]);
                    s.sorData.windSpeedMs = std::stof(cols[20]);
                } catch (...) {}
                s.sorData.sprayerModel = cols[21];
                s.sorData.sprayerInspectionNo = cols[22];
                s.sorData.sprayerInspectionValidUntil = cols[23];
                s.sorData.operatorName = cols[24];
                if (cols.size() >= 26) s.sorData.operatorCertificateNo = cols[25];
                s.sorData.isCompleted = true;

                taskHistory.push_back(s);
            }
        }
    }

    void TcBasReportManager::save_history_to_disk()
    {
        if (taskHistory.empty() && currentTask.workedAreaHa <= 0.0f) return;
        export_sor_csv_register("Raporty_SOR");
    }

} // namespace isobus_vt
