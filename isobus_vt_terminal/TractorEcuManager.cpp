#include "TractorEcuManager.hpp"
#include "FieldNavigation.hpp"
#include "TaskControllerManager.hpp"

#include "imgui.h"
#include <gdiplus.h>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>

namespace isobus_vt
{
    static std::uint32_t get_current_time_ms()
    {
        return static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    TractorEcuManager& TractorEcuManager::get_instance()
    {
        static TractorEcuManager instance;
        return instance;
    }

    TractorEcuManager::TractorEcuManager()
    {
        // Inicjalizacja GDI+ do ładowania tekstur PNG z przezroczystością
        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) == Gdiplus::Ok)
        {
            gdiplusInitialized = true;
        }

        init_models_list();
        load_brand_profile(TractorBrandProfile::JohnDeere);
    }

    TractorEcuManager::~TractorEcuManager()
    {
        stop();
        if (currentTractorTexture)
        {
            currentTractorTexture->Release();
            currentTractorTexture = nullptr;
        }
        if (currentBrandTexture)
        {
            currentBrandTexture->Release();
            currentBrandTexture = nullptr;
        }
        if (gdiplusInitialized)
        {
            Gdiplus::GdiplusShutdown(gdiplusToken);
            gdiplusInitialized = false;
        }
    }

    void TractorEcuManager::init_models_list()
    {
        tractorModels = {
            { "John Deere 6R / 8R", "TractorSideJohnDeere.png", "BrandJohnDeere.png", TractorBrandProfile::JohnDeere },
            { "Fendt Vario 700 / 900", "TractorSideFendt.png", "BrandFendt.png", TractorBrandProfile::FendtVario },
            { "Case IH Puma / Magnum", "TractorSideCase.png", "BrandCase.png", TractorBrandProfile::CaseNewHolland },
            { "Claas Axion / Arion", "TractorSideClaas.png", "BrandClaas.png", TractorBrandProfile::Claas },
            { "New Holland T7 / T8", "TractorSideNewHolland.png", "BrandNewHolland.png", TractorBrandProfile::CaseNewHolland },
            { "Deutz-Fahr Agrotron Series", "TractorSideDeutz.png", "BrandDeutz.png", TractorBrandProfile::DeutzFahr },
            { "Valtra T / S Series", "TractorSideValtra.png", "BrandValtra.png", TractorBrandProfile::StandardIsoJ1939 },
            { "Massey Ferguson 7S / 8S", "TractorSideMassey.png", "BrandMassey.png", TractorBrandProfile::StandardIsoJ1939 },
            { "Ursus C-360 / 1614", "TractorSideUrsus.png", "BrandUrsus.png", TractorBrandProfile::StandardIsoJ1939 },
            { "Steyr Profi / Terrus", "TractorSideSteyr.png", "BrandSteyr.png", TractorBrandProfile::CaseNewHolland },
            { "Kubota M7 / M8", "TractorSideKubota.png", "BrandKubota.png", TractorBrandProfile::StandardIsoJ1939 },
            { "JCB Fastrac", "TractorSideJCB.png", "BrandJCB.png", TractorBrandProfile::StandardIsoJ1939 },
            { "Same Iron / Fortis", "TractorSideSame.png", "BrandSame.png", TractorBrandProfile::DeutzFahr },
            { "AgOpenGPS Classic", "TractorSideAoG.png", "BrandAoG.png", TractorBrandProfile::StandardIsoJ1939 }
        };
    }

    void TractorEcuManager::set_d3d_device(ID3D11Device* device)
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        d3dDevice = device;
        if (d3dDevice && (!currentTractorTexture || !currentBrandTexture))
        {
            load_model_texture(selectedModelIndex);
        }
    }

    void TractorEcuManager::set_selected_model_index(int index)
    {
        if (index >= 0 && index < static_cast<int>(tractorModels.size()))
        {
            selectedModelIndex = index;
            auto prof = tractorModels[index].profile;
            {
                std::lock_guard<std::mutex> lock(dataMutex);
                currentProfile = prof;
                init_default_configs(prof);
            }
            load_model_texture(index);
        }
    }

    static std::string find_file_in_search_paths(const std::string &fileName)
    {
        std::vector<std::string> searchPaths = {
            "tractor_icons/" + fileName,
            "isobus_vt_terminal/tractor_icons/" + fileName,
            "../isobus_vt_terminal/tractor_icons/" + fileName,
            "../../isobus_vt_terminal/tractor_icons/" + fileName
        };

        for (const auto &p : searchPaths)
        {
            if (std::filesystem::exists(p)) return p;
        }
        return fileName;
    }

    ID3D11ShaderResourceView* TractorEcuManager::load_texture_from_file(const std::string &path, int &outW, int &outH)
    {
        if (!d3dDevice || !gdiplusInitialized) return nullptr;

        std::string resolvedPath = find_file_in_search_paths(path);
        std::wstring wPath(resolvedPath.begin(), resolvedPath.end());

        Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromFile(wPath.c_str());
        if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok)
        {
            delete bmp;
            return nullptr;
        }

        outW = static_cast<int>(bmp->GetWidth());
        outH = static_cast<int>(bmp->GetHeight());

        Gdiplus::Rect rect(0, 0, outW, outH);
        Gdiplus::BitmapData bmpData;
        if (bmp->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bmpData) != Gdiplus::Ok)
        {
            delete bmp;
            return nullptr;
        }

        std::vector<uint32_t> rgba(outW * outH);
        const uint32_t* src = static_cast<const uint32_t*>(bmpData.Scan0);
        int stride = bmpData.Stride / 4;

        for (int y = 0; y < outH; ++y)
        {
            for (int x = 0; x < outW; ++x)
            {
                uint32_t c = src[y * stride + x];
                uint32_t a = (c >> 24) & 0xFF;
                uint32_t r = (c >> 16) & 0xFF;
                uint32_t g = (c >> 8) & 0xFF;
                uint32_t b = (c) & 0xFF;
                rgba[y * outW + x] = (a << 24) | (b << 16) | (g << 8) | r;
            }
        }
        bmp->UnlockBits(&bmpData);
        delete bmp;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = outW;
        desc.Height = outH;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA sub = {};
        sub.pSysMem = rgba.data();
        sub.SysMemPitch = outW * 4;

        ID3D11Texture2D* pTexture = nullptr;
        HRESULT hr = d3dDevice->CreateTexture2D(&desc, &sub, &pTexture);
        if (FAILED(hr) || !pTexture) return nullptr;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = desc.MipLevels;

        ID3D11ShaderResourceView* pSRV = nullptr;
        hr = d3dDevice->CreateShaderResourceView(pTexture, &srvDesc, &pSRV);
        pTexture->Release();
        return pSRV;
    }

    void TractorEcuManager::load_model_texture(int modelIdx)
    {
        if (modelIdx < 0 || modelIdx >= static_cast<int>(tractorModels.size())) return;
        const auto &model = tractorModels[modelIdx];

        if (currentTractorTexture)
        {
            currentTractorTexture->Release();
            currentTractorTexture = nullptr;
        }
        if (currentBrandTexture)
        {
            currentBrandTexture->Release();
            currentBrandTexture = nullptr;
        }

        currentTractorTexture = load_texture_from_file(model.fileName, textureWidth, textureHeight);
        currentBrandTexture = load_texture_from_file(model.brandLogoFile, logoWidth, logoHeight);
    }

    bool TractorEcuManager::start()
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        if (!enabled)
        {
            isConnected = false;
            statusText = "Wylaczony w konfiguracji";
            return false;
        }
        if (currentSource == TractorCanSource::SecondPcanUsb)
        {
            CAN_Uninitialize(pcanChannel);
            TPCANStatus status = CAN_Initialize(pcanChannel, pcanBaud, 0, 0, 0);
            if (status == PCAN_ERROR_OK)
            {
                isConnected = true;
                statusText = "PCAN-USB 2 (J1939): Polaczono";
                return true;
            }
            else
            {
                isConnected = false;
                char errBuf[256] = { 0 };
                CAN_GetErrorText(status, 0, errBuf);
                statusText = std::string("PCAN-USB 2 Blad: ") + errBuf;
                return false;
            }
        }
        else if (currentSource == TractorCanSource::SharedWithCan1)
        {
            isConnected = true;
            statusText = "Wspoldzielona magistrala ISOBUS (CAN 1)";
            return true;
        }
        else
        {
            isConnected = true;
            statusText = "Tryb Symulacji Ciagnika";
            return true;
        }
    }

    void TractorEcuManager::stop()
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        if (currentSource == TractorCanSource::SecondPcanUsb && isConnected)
        {
            CAN_Uninitialize(pcanChannel);
        }
        isConnected = false;
        statusText = "Rozlaczono";
    }

    void TractorEcuManager::set_source(TractorCanSource src)
    {
        if (currentSource != src)
        {
            stop();
            currentSource = src;
            start();
        }
    }

    void TractorEcuManager::set_pcan_channel(TPCANHandle channel)
    {
        if (pcanChannel != channel)
        {
            stop();
            pcanChannel = channel;
            start();
        }
    }

    void TractorEcuManager::set_pcan_baudrate(TPCANBaudrate baud)
    {
        if (pcanBaud != baud)
        {
            stop();
            pcanBaud = baud;
            start();
        }
    }

    std::string TractorEcuManager::get_status_string() const
    {
        return statusText;
    }

    void TractorEcuManager::load_brand_profile(TractorBrandProfile profile)
    {
        int targetModelIdx = 0;
        {
            std::lock_guard<std::mutex> lock(dataMutex);
            currentProfile = profile;
            init_default_configs(profile);

            switch (profile)
            {
                case TractorBrandProfile::JohnDeere: targetModelIdx = 0; break;
                case TractorBrandProfile::FendtVario: targetModelIdx = 1; break;
                case TractorBrandProfile::CaseNewHolland: targetModelIdx = 2; break;
                case TractorBrandProfile::Claas: targetModelIdx = 3; break;
                case TractorBrandProfile::DeutzFahr: targetModelIdx = 5; break;
                case TractorBrandProfile::Custom: targetModelIdx = 13; break;
                case TractorBrandProfile::StandardIsoJ1939: default: targetModelIdx = 5; break;
            }
            selectedModelIndex = targetModelIdx;
        }
        load_model_texture(targetModelIdx);
    }

    void TractorEcuManager::init_default_configs(TractorBrandProfile profile)
    {
        fieldConfigs.clear();

        if (profile == TractorBrandProfile::CaseNewHolland)
        {
            fieldConfigs.push_back({ "TUZ Tyl (Pozycja %)", 65093, 0, 1, 0.4f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "TUZ Przod (Pozycja %)", 65093, 1, 1, 0.4f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "TUZ Stan Roboczy", 65093, 2, 1, 1.0f, 0.0f, 0.0f, 3.0f, "bit", true });
            fieldConfigs.push_back({ "CNH Podnosnik UCM Status", 65093, 3, 1, 1.0f, 0.0f, 0.0f, 7.0f, "bit", true });
            fieldConfigs.push_back({ "CNH Hydraulika / Aux Zawor 1", 65092, 0, 2, 0.1f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "CNH Hydraulika / Aux Zawor 2", 65092, 2, 2, 0.1f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "WOM Tyl (Obroty RPM)", 65091, 0, 2, 0.125f, 0.0f, 0.0f, 2500.0f, "RPM", true });
            fieldConfigs.push_back({ "WOM Przod (Obroty RPM)", 65091, 2, 2, 0.125f, 0.0f, 0.0f, 2500.0f, "RPM", true });
            fieldConfigs.push_back({ "Obroty Silnika (RPM)", 61444, 3, 2, 0.125f, 0.0f, 0.0f, 3500.0f, "RPM", true });
            fieldConfigs.push_back({ "Obciazenie Silnika (%)", 61444, 2, 1, 1.0f, -125.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "Spalanie Chwilowe (l/h)", 65266, 0, 2, 0.05f, 0.0f, 0.0f, 150.0f, "l/h", true });
            fieldConfigs.push_back({ "Poziom Paliwa w Baku (%)", 65276, 1, 1, 0.4f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "Temperatura Plynu (C)", 65262, 0, 1, 1.0f, -40.0f, -40.0f, 125.0f, "C", true });
            fieldConfigs.push_back({ "Predkosc Radaru (km/h)", 65096, 0, 2, 0.0036f, 0.0f, 0.0f, 70.0f, "km/h", true });
            fieldConfigs.push_back({ "Predkosc Kol (km/h)", 65096, 2, 2, 0.0036f, 0.0f, 0.0f, 70.0f, "km/h", true });
        }
        else if (profile == TractorBrandProfile::JohnDeere)
        {
            fieldConfigs.push_back({ "TUZ Tyl (Pozycja %)", 65093, 0, 1, 0.4f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "TUZ Przod (Pozycja %)", 65093, 1, 1, 0.4f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "TUZ Stan Roboczy", 65093, 2, 1, 1.0f, 0.0f, 0.0f, 3.0f, "bit", true });
            fieldConfigs.push_back({ "JD AutoTrac Hitch Status", 65300, 0, 1, 0.4f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "WOM Tyl (Obroty RPM)", 65091, 0, 2, 0.125f, 0.0f, 0.0f, 2500.0f, "RPM", true });
            fieldConfigs.push_back({ "WOM Przod (Obroty RPM)", 65091, 2, 2, 0.125f, 0.0f, 0.0f, 2500.0f, "RPM", true });
            fieldConfigs.push_back({ "Obroty Silnika (RPM)", 61444, 3, 2, 0.125f, 0.0f, 0.0f, 3500.0f, "RPM", true });
            fieldConfigs.push_back({ "Obciazenie Silnika (%)", 61444, 2, 1, 1.0f, -125.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "Spalanie Chwilowe (l/h)", 65266, 0, 2, 0.05f, 0.0f, 0.0f, 150.0f, "l/h", true });
            fieldConfigs.push_back({ "Poziom Paliwa w Baku (%)", 65276, 1, 1, 0.4f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "Temperatura Plynu (C)", 65262, 0, 1, 1.0f, -40.0f, -40.0f, 125.0f, "C", true });
            fieldConfigs.push_back({ "Predkosc Radaru (km/h)", 65096, 0, 2, 0.0036f, 0.0f, 0.0f, 70.0f, "km/h", true });
            fieldConfigs.push_back({ "Predkosc Kol (km/h)", 65096, 2, 2, 0.0036f, 0.0f, 0.0f, 70.0f, "km/h", true });
        }
        else if (profile == TractorBrandProfile::FendtVario)
        {
            fieldConfigs.push_back({ "TUZ Tyl (Fendt K-CAN)", 65280, 2, 2, 0.1f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "TUZ Stan Roboczy", 65280, 0, 1, 1.0f, 0.0f, 0.0f, 3.0f, "bit", true });
            fieldConfigs.push_back({ "WOM Tyl (Obroty RPM)", 65091, 0, 2, 0.125f, 0.0f, 0.0f, 2500.0f, "RPM", true });
            fieldConfigs.push_back({ "WOM Przod (Obroty RPM)", 65091, 2, 2, 0.125f, 0.0f, 0.0f, 2500.0f, "RPM", true });
            fieldConfigs.push_back({ "Obroty Silnika (RPM)", 61444, 3, 2, 0.125f, 0.0f, 0.0f, 3500.0f, "RPM", true });
            fieldConfigs.push_back({ "Obciazenie Silnika (%)", 61444, 2, 1, 1.0f, -125.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "Spalanie Chwilowe (l/h)", 65266, 0, 2, 0.05f, 0.0f, 0.0f, 150.0f, "l/h", true });
            fieldConfigs.push_back({ "Poziom Paliwa w Baku (%)", 65276, 1, 1, 0.4f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "Temperatura Plynu (C)", 65262, 0, 1, 1.0f, -40.0f, -40.0f, 125.0f, "C", true });
            fieldConfigs.push_back({ "Predkosc Radaru (km/h)", 65096, 0, 2, 0.0036f, 0.0f, 0.0f, 70.0f, "km/h", true });
            fieldConfigs.push_back({ "Predkosc Kol (km/h)", 65096, 2, 2, 0.0036f, 0.0f, 0.0f, 70.0f, "km/h", true });
        }
        else if (profile == TractorBrandProfile::Claas)
        {
            fieldConfigs.push_back({ "TUZ Tyl (Pozycja %)", 65093, 0, 1, 0.4f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "TUZ Stan Roboczy", 65093, 2, 1, 1.0f, 0.0f, 0.0f, 3.0f, "bit", true });
            fieldConfigs.push_back({ "Claas Cebis Przelacznik", 65281, 1, 1, 1.0f, 0.0f, 0.0f, 3.0f, "bit", true });
            fieldConfigs.push_back({ "WOM Tyl (Obroty RPM)", 65091, 0, 2, 0.125f, 0.0f, 0.0f, 2500.0f, "RPM", true });
            fieldConfigs.push_back({ "Obroty Silnika (RPM)", 61444, 3, 2, 0.125f, 0.0f, 0.0f, 3500.0f, "RPM", true });
            fieldConfigs.push_back({ "Obciazenie Silnika (%)", 61444, 2, 1, 1.0f, -125.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "Spalanie Chwilowe (l/h)", 65266, 0, 2, 0.05f, 0.0f, 0.0f, 150.0f, "l/h", true });
            fieldConfigs.push_back({ "Poziom Paliwa w Baku (%)", 65276, 1, 1, 0.4f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "Temperatura Plynu (C)", 65262, 0, 1, 1.0f, -40.0f, -40.0f, 125.0f, "C", true });
            fieldConfigs.push_back({ "Predkosc Radaru (km/h)", 65096, 0, 2, 0.0036f, 0.0f, 0.0f, 70.0f, "km/h", true });
            fieldConfigs.push_back({ "Predkosc Kol (km/h)", 65096, 2, 2, 0.0036f, 0.0f, 0.0f, 70.0f, "km/h", true });
        }
        else if (profile == TractorBrandProfile::DeutzFahr)
        {
            // Deutz-Fahr / Same (SDF Agrotron / TTV / Profiline / EMR3 / EMR4 / EDC17)
            fieldConfigs.push_back({ "TUZ Tyl (ISO HPC %)", 65093, 0, 1, 0.4f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "TUZ Przod (ISO HPC %)", 65093, 1, 1, 0.4f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "TUZ Stan Roboczy (In-Work)", 65093, 2, 1, 1.0f, 0.0f, 0.0f, 3.0f, "bit", true });
            fieldConfigs.push_back({ "SDF EHR Pozycja K-CAN (%)", 65282, 0, 1, 0.4f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "SDF AgroSky / iMonitor Przelacznik Roboczy", 65302, 0, 1, 1.0f, 0.0f, 0.0f, 3.0f, "bit", true });
            fieldConfigs.push_back({ "WOM Tyl (Obroty RPM)", 65091, 0, 2, 0.125f, 0.0f, 0.0f, 2500.0f, "RPM", true });
            fieldConfigs.push_back({ "WOM Przod (Obroty RPM)", 65091, 2, 2, 0.125f, 0.0f, 0.0f, 2500.0f, "RPM", true });
            fieldConfigs.push_back({ "Obroty Silnika Deutz TCD (RPM)", 61444, 3, 2, 0.125f, 0.0f, 0.0f, 3500.0f, "RPM", true });
            fieldConfigs.push_back({ "Obciazenie Silnika Deutz (%)", 61444, 2, 1, 1.0f, -125.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "Spalanie Chwilowe Deutz (l/h)", 65266, 0, 2, 0.05f, 0.0f, 0.0f, 150.0f, "l/h", true });
            fieldConfigs.push_back({ "Poziom Paliwa w Baku (%)", 65276, 1, 1, 0.4f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "Temperatura Plynu Deutz (C)", 65262, 0, 1, 1.0f, -40.0f, -40.0f, 125.0f, "C", true });
            fieldConfigs.push_back({ "Temperatura Oleju Silnika (C)", 65262, 2, 2, 0.03125f, -273.0f, -40.0f, 150.0f, "C", true });
            fieldConfigs.push_back({ "Cisnienie Doladowania Turbo (kPa)", 65270, 1, 1, 2.0f, 0.0f, 0.0f, 500.0f, "kPa", true });
            fieldConfigs.push_back({ "Predkosc Radaru (km/h)", 65096, 0, 2, 0.0036f, 0.0f, 0.0f, 70.0f, "km/h", true });
            fieldConfigs.push_back({ "Predkosc Kol (km/h)", 65096, 2, 2, 0.0036f, 0.0f, 0.0f, 70.0f, "km/h", true });
            fieldConfigs.push_back({ "SDF Hydraulika / Aux Zawor 1 (%)", 65092, 0, 2, 0.1f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "SDF Hydraulika / Aux Zawor 2 (%)", 65092, 2, 2, 0.1f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "Motogodziny Silnika (h)", 65248, 4, 4, 0.05f, 0.0f, 0.0f, 50000.0f, "h", true });
        }
        else
        {
            // Standard ISO 11783-7 & SAE J1939
            fieldConfigs.push_back({ "TUZ Tyl (Pozycja %)", 65093, 0, 1, 0.4f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "TUZ Przod (Pozycja %)", 65093, 1, 1, 0.4f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "TUZ Stan Roboczy", 65093, 2, 1, 1.0f, 0.0f, 0.0f, 3.0f, "bit", true });
            fieldConfigs.push_back({ "WOM Tyl (Obroty RPM)", 65091, 0, 2, 0.125f, 0.0f, 0.0f, 2500.0f, "RPM", true });
            fieldConfigs.push_back({ "WOM Przod (Obroty RPM)", 65091, 2, 2, 0.125f, 0.0f, 0.0f, 2500.0f, "RPM", true });
            fieldConfigs.push_back({ "Obroty Silnika (RPM)", 61444, 3, 2, 0.125f, 0.0f, 0.0f, 3500.0f, "RPM", true });
            fieldConfigs.push_back({ "Obciazenie Silnika (%)", 61444, 2, 1, 1.0f, -125.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "Spalanie Chwilowe (l/h)", 65266, 0, 2, 0.05f, 0.0f, 0.0f, 150.0f, "l/h", true });
            fieldConfigs.push_back({ "Poziom Paliwa w Baku (%)", 65276, 1, 1, 0.4f, 0.0f, 0.0f, 100.0f, "%", true });
            fieldConfigs.push_back({ "Temperatura Plynu (C)", 65262, 0, 1, 1.0f, -40.0f, -40.0f, 125.0f, "C", true });
            fieldConfigs.push_back({ "Predkosc Radaru (km/h)", 65096, 0, 2, 0.0036f, 0.0f, 0.0f, 70.0f, "km/h", true });
            fieldConfigs.push_back({ "Predkosc Kol (km/h)", 65096, 2, 2, 0.0036f, 0.0f, 0.0f, 70.0f, "km/h", true });
        }
    }

    void TractorEcuManager::update()
    {
        if (!enabled) return;
        if (autoDetectState == AutoDetectState::Scanning)
        {
            update_auto_detect();
            return;
        }

        if (currentSource == TractorCanSource::SecondPcanUsb && isConnected)
        {
            TPCANMsg msg;
            TPCANTimestamp timestamp;
            std::uint32_t count = 0;

            while (count++ < 200 && CAN_Read(pcanChannel, &msg, &timestamp) == PCAN_ERROR_OK)
            {
                if (msg.MSGTYPE & PCAN_MESSAGE_EXTENDED)
                {
                    process_can_frame(msg.ID, msg.DATA, msg.LEN);
                }
            }
        }
        else if (currentSource == TractorCanSource::SimulationMode)
        {
            update_simulation();
        }

        // Automatyczna synchronizacja stanu pracy (TUZ / WOM) z FieldNavigation
        bool workActive = is_work_active();
        if (autoWorkTriggerEnabled)
        {
            FieldNavigation::get_instance().set_master_work_switch(workActive);
        }
    }

    void TractorEcuManager::process_can_frame(std::uint32_t id, const std::uint8_t *data, std::uint8_t len)
    {
        if (!enabled) return;
        std::lock_guard<std::mutex> lock(dataMutex);

        std::uint32_t pgn = (id >> 8) & 0x03FFFF;
        std::uint8_t sa = static_cast<std::uint8_t>(id & 0xFF);
        std::uint32_t now = get_current_time_ms();

        // Aktualizacja sniffowanych ramek
        auto &entry = sniffedFrames[pgn];
        entry.pgn = pgn;
        entry.sa = sa;
        entry.dlc = len;
        entry.count++;
        entry.lastTimeMs = now;

        for (int i = 0; i < 8 && i < len; ++i)
        {
            if (entry.data[i] != data[i])
            {
                entry.prevData[i] = entry.data[i];
                entry.byteChanged[i] = true;
            }
            else
            {
                entry.byteChanged[i] = false;
            }
            entry.data[i] = data[i];
        }

        decode_frame(pgn, sa, data, len);

        // Aktualizacja listy wykrytych sterowników (Auto-Sniffer)
        auto &ecu = discoveredEcus[sa];
        ecu.sa = sa;
        if (ecu.name.empty()) ecu.name = get_ecu_name(sa);
        ecu.frameCount++;
        if (now > ecu.lastSeenMs && ecu.lastSeenMs > 0)
        {
            float dt = (now - ecu.lastSeenMs) / 1000.0f;
            if (dt > 0.001f)
            {
                float instantRate = 1.0f / dt;
                ecu.frameRateHz = ecu.frameRateHz * 0.85f + instantRate * 0.15f;
            }
        }
        ecu.lastSeenMs = now;
        if (std::find(ecu.observedPgns.begin(), ecu.observedPgns.end(), pgn) == ecu.observedPgns.end())
        {
            if (ecu.observedPgns.size() < 16) ecu.observedPgns.push_back(pgn);
        }
    }

    void TractorEcuManager::decode_frame(std::uint32_t pgn, std::uint8_t sa, const std::uint8_t *data, std::uint8_t len)
    {
        std::uint32_t now = get_current_time_ms();
        telemetry.lastMessageTimeMs = now;
        telemetry.totalFramesReceived++;

        // Dynamic decode from user fieldConfigs
        for (const auto &cfg : fieldConfigs)
        {
            if (cfg.pgn != pgn) continue;
            if (cfg.startByte >= len) continue;

            uint32_t rawVal = 0;
            if (cfg.lengthBytes == 1)
            {
                rawVal = data[cfg.startByte];
                if (rawVal > 250 && cfg.unit == "%") continue;
            }
            else if (cfg.lengthBytes == 2)
            {
                if (cfg.startByte + 1 < len)
                {
                    rawVal = static_cast<uint32_t>(data[cfg.startByte]) | (static_cast<uint32_t>(data[cfg.startByte + 1]) << 8);
                    if (rawVal == 0xFFFF) continue;
                }
                else rawVal = data[cfg.startByte];
            }
            else if (cfg.lengthBytes == 4)
            {
                if (cfg.startByte + 3 < len)
                {
                    rawVal = static_cast<uint32_t>(data[cfg.startByte]) |
                             (static_cast<uint32_t>(data[cfg.startByte + 1]) << 8) |
                             (static_cast<uint32_t>(data[cfg.startByte + 2]) << 16) |
                             (static_cast<uint32_t>(data[cfg.startByte + 3]) << 24);
                    if (rawVal == 0xFFFFFFFF) continue;
                }
            }

            float val = (static_cast<float>(rawVal) * cfg.scale) + cfg.offset;

            if (cfg.name.find("TUZ Tyl") != std::string::npos || cfg.name.find("Hitch Proprietary") != std::string::npos || cfg.name.find("SDF EHR") != std::string::npos)
            {
                telemetry.rearHitchPositionPct = std::clamp(val, 0.0f, 100.0f);
                telemetry.isRearHitchLowered = (telemetry.rearHitchPositionPct <= hitchLoweredThresholdPct);
            }
            else if (cfg.name.find("TUZ Przod") != std::string::npos)
            {
                telemetry.frontHitchPositionPct = std::clamp(val, 0.0f, 100.0f);
                telemetry.isFrontHitchLowered = (telemetry.frontHitchPositionPct <= hitchLoweredThresholdPct);
            }
            else if (cfg.name.find("TUZ Stan Roboczy") != std::string::npos || cfg.name.find("Przelacznik") != std::string::npos || cfg.name.find("UCM Status") != std::string::npos || cfg.name.find("Roboczy") != std::string::npos)
            {
                telemetry.hitchStatusBitfield = static_cast<uint8_t>(rawVal);
                if (rawVal == 1 || (rawVal & 0x01))
                {
                    telemetry.isRearHitchLowered = true;
                }
            }
            else if (cfg.name.find("WOM Tyl") != std::string::npos)
            {
                telemetry.rearPtoRpm = std::max(0.0f, val);
                telemetry.isRearPtoActive = (telemetry.rearPtoRpm >= 100.0f);
            }
            else if (cfg.name.find("WOM Przod") != std::string::npos)
            {
                telemetry.frontPtoRpm = std::max(0.0f, val);
                telemetry.isFrontPtoActive = (telemetry.frontPtoRpm >= 100.0f);
            }
            else if (cfg.name.find("Obroty Silnika") != std::string::npos)
            {
                telemetry.engineRpm = std::max(0.0f, val);
            }
            else if (cfg.name.find("Obciazenie") != std::string::npos)
            {
                telemetry.engineLoadPct = std::clamp(val, 0.0f, 100.0f);
            }
            else if (cfg.name.find("Spalanie") != std::string::npos)
            {
                telemetry.fuelRateLitersPerHour = std::max(0.0f, val);
                if (telemetry.groundSpeedKph > 1.0f)
                {
                    float haPerHour = (telemetry.groundSpeedKph * 12.0f) / 10.0f;
                    if (haPerHour > 0.1f)
                    {
                        telemetry.avgFuelEconomyLitersPerHa = telemetry.fuelRateLitersPerHour / haPerHour;
                    }
                }
            }
            else if (cfg.name.find("Poziom Paliwa") != std::string::npos)
            {
                telemetry.fuelLevelPct = std::clamp(val, 0.0f, 100.0f);
            }
            else if (cfg.name.find("Temperatura Plynu") != std::string::npos)
            {
                telemetry.coolantTempC = val;
            }
            else if (cfg.name.find("Temperatura Oleju") != std::string::npos)
            {
                telemetry.oilTempC = val;
            }
            else if (cfg.name.find("Doladowani") != std::string::npos || cfg.name.find("Turbo") != std::string::npos)
            {
                telemetry.boostPressureKpa = val;
            }
            else if (cfg.name.find("Motogodziny") != std::string::npos)
            {
                telemetry.engineHours = val;
            }
            else if (cfg.name.find("Predkosc Radaru") != std::string::npos)
            {
                telemetry.groundSpeedKph = std::max(0.0f, val);
            }
            else if (cfg.name.find("Predkosc Kol") != std::string::npos)
            {
                telemetry.wheelSpeedKph = std::max(0.0f, val);
                if (telemetry.wheelSpeedKph > 1.0f && telemetry.groundSpeedKph > 0.1f)
                {
                    telemetry.wheelSlipPct = std::clamp(((telemetry.wheelSpeedKph - telemetry.groundSpeedKph) / telemetry.wheelSpeedKph) * 100.0f, 0.0f, 100.0f);
                }
                else
                {
                    telemetry.wheelSlipPct = 0.0f;
                }
            }
        }

        telemetry.isWorkConditionMet = telemetry.isRearHitchLowered || telemetry.isRearPtoActive;
    }

    void TractorEcuManager::update_simulation()
    {
        std::uint32_t now = get_current_time_ms();
        static std::uint32_t lastSimTime = 0;
        if (now - lastSimTime < 80) return; // ~12 Hz simulated stream
        lastSimTime = now;

        // 1. Pack and process Hitch frame (PGN from fieldConfigs or default 65093)
        std::uint32_t hitchPgn = 65093;
        uint8_t hitchByte = 0;
        float hitchScale = 0.4f;
        for (const auto &c : fieldConfigs)
        {
            if (c.name.find("TUZ Tyl") != std::string::npos)
            {
                hitchPgn = c.pgn;
                hitchByte = c.startByte;
                if (c.scale > 0.0001f) hitchScale = c.scale;
                break;
            }
        }
        uint8_t hitchData[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        if (hitchByte < 8)
        {
            hitchData[hitchByte] = static_cast<uint8_t>(std::clamp(simHitch / hitchScale, 0.0f, 250.0f));
        }
        if (hitchByte + 1 < 8) hitchData[hitchByte + 1] = 250;
        if (hitchByte + 2 < 8) hitchData[hitchByte + 2] = (simHitch <= hitchLoweredThresholdPct) ? 0x01 : 0x00;
        process_can_frame(0x18000027 | (hitchPgn << 8), hitchData, 8);

        // 2. PTO PGN 65091 (0xFE43)
        uint8_t ptoData[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        uint16_t rawPto = static_cast<uint16_t>(simPto / 0.125f);
        ptoData[0] = static_cast<uint8_t>(rawPto & 0xFF);
        ptoData[1] = static_cast<uint8_t>((rawPto >> 8) & 0xFF);
        process_can_frame(0x18FE4327, ptoData, 8);

        // 3. Engine PGN 61444 (0xF004)
        uint8_t engineData[8] = { 0xFF, 0xFF, static_cast<uint8_t>(52 + 125), 0, 0, 0xFF, 0xFF, 0xFF };
        uint16_t rawRpm = static_cast<uint16_t>(simEngineRpm / 0.125f);
        engineData[3] = static_cast<uint8_t>(rawRpm & 0xFF);
        engineData[4] = static_cast<uint8_t>((rawRpm >> 8) & 0xFF);
        process_can_frame(0x0CF00400, engineData, 8);

        // 4. Fuel Rate PGN 65266 (0xFEF2)
        uint8_t fuelData[8] = { 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        uint16_t rawFuel = static_cast<uint16_t>(simFuelRate / 0.05f);
        fuelData[0] = static_cast<uint8_t>(rawFuel & 0xFF);
        fuelData[1] = static_cast<uint8_t>((rawFuel >> 8) & 0xFF);
        process_can_frame(0x18FEF200, fuelData, 8);

        // 5. Fuel Level PGN 65276 (0xFEFC)
        uint8_t tankData[8] = { 0xFF, static_cast<uint8_t>(simFuelLevel / 0.4f), 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        process_can_frame(0x18FEFC00, tankData, 8);

        // 6. Coolant Temp PGN 65262 (0xFEEE)
        uint8_t tempData[8] = { static_cast<uint8_t>(std::clamp(simCoolant + 40.0f, 0.0f, 250.0f)), 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        process_can_frame(0x18FEEE00, tempData, 8);

        // 7. Speed PGN 65096 (0xFE48)
        uint8_t speedData[8] = { 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF };
        uint16_t rawGround = static_cast<uint16_t>(8.5f / 0.0036f);
        uint16_t rawWheel = static_cast<uint16_t>(8.9f / 0.0036f);
        speedData[0] = static_cast<uint8_t>(rawGround & 0xFF);
        speedData[1] = static_cast<uint8_t>((rawGround >> 8) & 0xFF);
        speedData[2] = static_cast<uint8_t>(rawWheel & 0xFF);
        speedData[3] = static_cast<uint8_t>((rawWheel >> 8) & 0xFF);
        process_can_frame(0x18FE4827, speedData, 8);
    }

    TractorLiveTelemetry TractorEcuManager::get_telemetry()
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        return telemetry;
    }

    bool TractorEcuManager::is_work_active() const
    {
        return telemetry.isWorkConditionMet;
    }

    std::vector<SniffedCanFrame> TractorEcuManager::get_sniffed_frames()
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        std::vector<SniffedCanFrame> result;
        result.reserve(sniffedFrames.size());
        for (const auto &pair : sniffedFrames)
        {
            result.push_back(pair.second);
        }
        return result;
    }

    void TractorEcuManager::clear_sniffer()
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        sniffedFrames.clear();
    }

    std::string TractorEcuManager::get_ecu_name(std::uint8_t sa)
    {
        switch (sa)
        {
            case 0x00: return "Silnik Deutz (Engine / EMR4 / EDC)";
            case 0x01: return "Silnik pomocniczy";
            case 0x02: return "Turbosprezarka";
            case 0x03: return "Skrzynia biegow (Transmission / TTV)";
            case 0x07: return "Sterowanie osiami / Zawieszenie";
            case 0x0B: return "Uklad hamulcowy (Brakes)";
            case 0x10: return "Nawigacja satelitarna (GPS)";
            case 0x11: return "Uklad kierowniczy (Autosteer)";
            case 0x13: return "Tylny podnosnik TUZ (Rear Hitch)";
            case 0x17: return "Zestaw wskaznikow (Infocenter / Dashboard)";
            case 0x19: return "Zawory hydrauliczne SCV";
            case 0x1C: return "Przedni podnosnik TUZ (Front Hitch)";
            case 0x1D: return "Przedni walek WOM (Front PTO)";
            case 0x27: return "Zarzadca ciagnika (Tractor ECU / TECU)";
            case 0x32: return "Modul Agrosky / Antena GPS";
            case 0x81: return "Terminal ISOBUS (Virtual Terminal)";
            case 0xCE: return "Amazone Amatron / AmaClick";
            case 0xF0: return "AgOpenGPS Virtual TECU";
            default:
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "Sterownik J1939 (Adres 0x%02X)", sa);
                return std::string(buf);
            }
        }
    }

    void TractorEcuManager::start_auto_detect()
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        currentSource = TractorCanSource::SecondPcanUsb;
        autoDetectState = AutoDetectState::Scanning;
        currentScanBaudIdx = 0;
        scanStartTimeMs = get_current_time_ms();
        scanFrameCount = 0;
        autoDetectResultText = "Testowanie: 250 kbit/s (Standard J1939 / ISOBUS)...";

        CAN_Uninitialize(pcanChannel);
        TPCANStatus st = CAN_Initialize(pcanChannel, PCAN_BAUD_250K, 0, 0, 0);
        if (st == PCAN_ERROR_OK)
        {
            isConnected = true;
        }
    }

    void TractorEcuManager::stop_auto_detect()
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        autoDetectState = AutoDetectState::Idle;
        autoDetectResultText.clear();
    }

    bool TractorEcuManager::is_auto_detecting() const
    {
        return autoDetectState == AutoDetectState::Scanning;
    }

    void TractorEcuManager::update_auto_detect()
    {
        const TPCANBaudrate bauds[] = { PCAN_BAUD_250K, PCAN_BAUD_500K, PCAN_BAUD_125K, PCAN_BAUD_1M };
        const char* baudNames[] = { "250 kbit/s (Standard J1939)", "500 kbit/s (Silnik Deutz EMR4 / High-Speed)", "125 kbit/s (Deska / Panel)", "1 Mbit/s" };
        const int numBauds = 4;

        std::uint32_t now = get_current_time_ms();

        // 1. Probuj czytac ramki
        TPCANMsg msg;
        TPCANTimestamp ts;
        int readInCycle = 0;
        while (readInCycle++ < 50 && CAN_Read(pcanChannel, &msg, &ts) == PCAN_ERROR_OK)
        {
            if (msg.MSGTYPE & PCAN_MESSAGE_EXTENDED)
            {
                scanFrameCount++;
                process_can_frame(msg.ID, msg.DATA, msg.LEN);
            }
        }

        // Jezeli odebrano co najmniej 2 ramki -> SUKCES!
        if (scanFrameCount >= 2)
        {
            autoDetectState = AutoDetectState::Success;
            pcanBaud = bauds[currentScanBaudIdx];
            char buf[256];
            snprintf(buf, sizeof(buf), "Wykryto magistrale: %s! Odebrano %u ramek.", baudNames[currentScanBaudIdx], scanFrameCount);
            autoDetectResultText = buf;
            statusText = std::string("PCAN-USB (J1939): Polaczono - ") + baudNames[currentScanBaudIdx];
            isConnected = true;
            return;
        }

        // Jezeli minelo 600 ms na dany baudrate bez ramek -> przelacz na nastepny
        if (now - scanStartTimeMs > 600)
        {
            CAN_Uninitialize(pcanChannel);
            currentScanBaudIdx++;
            if (currentScanBaudIdx < numBauds)
            {
                scanStartTimeMs = now;
                scanFrameCount = 0;
                autoDetectResultText = std::string("Testowanie: ") + baudNames[currentScanBaudIdx] + "...";
                CAN_Initialize(pcanChannel, bauds[currentScanBaudIdx], 0, 0, 0);
            }
            else
            {
                // Przetestowano wszystkie i cisza
                autoDetectState = AutoDetectState::Failed;
                autoDetectResultText = "Brak ramek na wybranym kanale PCAN! Sprawdz: 1) Zaplon ciagnika ON, 2) CAN_H pin 4, CAN_L pin 3, 3) Czy to wlasciwe gniazdo.";
                pcanBaud = PCAN_BAUD_250K;
                CAN_Initialize(pcanChannel, pcanBaud, 0, 0, 0);
            }
        }
    }

    std::vector<DiscoveredEcu> TractorEcuManager::get_discovered_ecus()
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        std::vector<DiscoveredEcu> result;
        result.reserve(discoveredEcus.size());
        for (const auto &pair : discoveredEcus)
        {
            result.push_back(pair.second);
        }
        std::sort(result.begin(), result.end(), [](const DiscoveredEcu &a, const DiscoveredEcu &b) {
            return a.frameCount > b.frameCount;
        });
        return result;
    }


    // =========================================================================
    // GUI: Pulpit Ciągnika (Tractor Dashboard z grafiką AgOpenGPS)
    // =========================================================================
    void TractorEcuManager::render_tractor_dashboard(bool *p_open)
    {
        if (p_open && !*p_open) return;

        // Skalowanie: Wygodne proporcje, brak ucinania tekstu
        ImGui::SetNextWindowSizeConstraints(ImVec2(600, 580), ImVec2(1920, 1080));
        ImGui::SetNextWindowSize(ImVec2(680, 660), ImGuiCond_FirstUseEver);

        if (!ImGui::Begin("Telemetria Ciagnika (TECU / J1939)", p_open))
        {
            ImGui::End();
            return;
        }

        // Lazy load texture jeśli urządzenie Direct3D jest gotowe
        if (d3dDevice && (!currentTractorTexture || !currentBrandTexture))
        {
            load_model_texture(selectedModelIndex);
        }

        TractorLiveTelemetry t = get_telemetry();
        std::uint32_t now = get_current_time_ms();
        bool isOnline = (now - t.lastMessageTimeMs) < 2500;

        // Pasek statusu u góry
        ImGui::PushStyleColor(ImGuiCol_Text, isOnline ? ImVec4(0.2f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        ImGui::Text(isOnline ? "[ TECU: ONLINE ]" : "[ TECU: OFFLINE / BRAK DANYCH ]");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("| Mag: %s", (currentSource == TractorCanSource::SecondPcanUsb) ? "PCAN-USB 2" : (currentSource == TractorCanSource::SharedWithCan1 ? "CAN 1 (Shared)" : "SYMULACJA"));
        ImGui::SameLine();
        ImGui::Text("| Ramki: %llu", t.totalFramesReceived);

        // ── Auto-Baudrate & Auto-Sniffer Controls ──
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "Zrodlo CAN:");
        ImGui::SameLine();
        int srcChoice = static_cast<int>(currentSource);
        ImGui::SetNextItemWidth(170.0f);
        if (ImGui::Combo("##src_combo", &srcChoice, "PCAN-USB 2 (Dedykowany)\0CAN 1 (Wspoldzielony ISOBUS)\0Symulacja (Test na stole)\0\0"))
        {
            set_source(static_cast<TractorCanSource>(srcChoice));
        }

        if (currentSource == TractorCanSource::SecondPcanUsb)
        {
            ImGui::SameLine();
            ImGui::Text("Kanal:");
            ImGui::SameLine();
            int chIdx = (pcanChannel >= PCAN_USBBUS1 && pcanChannel <= PCAN_USBBUS8) ? (pcanChannel - PCAN_USBBUS1) : 1;
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::Combo("##pcan_ch_quick", &chIdx, "USB 1\0USB 2\0USB 3\0USB 4\0\0"))
            {
                set_pcan_channel(static_cast<TPCANHandle>(PCAN_USBBUS1 + chIdx));
            }

            ImGui::SameLine();
            ImGui::Text("Baud:");
            ImGui::SameLine();
            int baudChoice = (pcanBaud == PCAN_BAUD_500K) ? 1 : ((pcanBaud == PCAN_BAUD_125K) ? 2 : 0);
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::Combo("##baud_quick", &baudChoice, "250 kbps\0500 kbps\0125 kbps\0\0"))
            {
                TPCANBaudrate b = (baudChoice == 1) ? PCAN_BAUD_500K : ((baudChoice == 2) ? PCAN_BAUD_125K : PCAN_BAUD_250K);
                set_pcan_baudrate(b);
            }

            ImGui::SameLine();
            if (is_auto_detecting())
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.45f, 0.1f, 1.0f));
                if (ImGui::Button("Skanowanie... (Zatrzymaj)"))
                {
                    stop_auto_detect();
                }
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.58f, 0.95f, 1.0f));
                if (ImGui::Button("🔍 Auto-Wykryj Baudrate & Ciagnik"))
                {
                    start_auto_detect();
                }
                ImGui::PopStyleColor();
            }

            if (!autoDetectResultText.empty())
            {
                if (autoDetectState == AutoDetectState::Scanning)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "⏳ %s", autoDetectResultText.c_str());
                }
                else if (autoDetectState == AutoDetectState::Success)
                {
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "✅ %s", autoDetectResultText.c_str());
                }
                else if (autoDetectState == AutoDetectState::Failed)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "❌ %s", autoDetectResultText.c_str());
                }
            }
        }

        ImGui::Separator();

        // 1. Menu Wyboru Ciągnika (Model Selector z AgOpenGPS)
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f), "Model Ciagnika:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::BeginCombo("##TractorModelCombo", tractorModels[selectedModelIndex].displayName.c_str()))
        {
            for (int i = 0; i < static_cast<int>(tractorModels.size()); ++i)
            {
                bool isSelected = (selectedModelIndex == i);
                if (ImGui::Selectable(tractorModels[i].displayName.c_str(), isSelected))
                {
                    set_selected_model_index(i);
                }
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        ImGui::Checkbox("Auto-malowanie przy opuszczonym TUZ", &autoWorkTriggerEnabled);

        // 2. Podgląd Ciągnika z grafiką AgOpenGPS i animowanym TUZ-em
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float canvasW = ImGui::GetContentRegionAvail().x;
        float canvasH = 220.0f;
        ImVec2 p1 = ImVec2(p0.x + canvasW, p0.y + canvasH);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Tło ramy
        drawList->AddRectFilled(p0, p1, IM_COL32(18, 22, 28, 255), 8.0f);
        drawList->AddRect(p0, p1, IM_COL32(50, 65, 85, 255), 8.0f, 0, 1.5f);

        // Wyświetlanie autentycznej grafiki AgOpenGPS traktora stojącego BOKIEM
        float tractorImgW = 270.0f;
        float tractorImgH = 184.0f;
        float imgX = p0.x + (canvasW - tractorImgW) * 0.5f;
        float imgY = p0.y + (canvasH - tractorImgH) * 0.5f + 10.0f;

        if (currentTractorTexture)
        {
            drawList->AddImage(
                reinterpret_cast<ImTextureID>(currentTractorTexture),
                ImVec2(imgX, imgY),
                ImVec2(imgX + tractorImgW, imgY + tractorImgH),
                ImVec2(0, 0), ImVec2(1, 1),
                IM_COL32(255, 255, 255, 255)
            );
        }

        // Logo marki w prawym górnym rogu podglądu
        if (currentBrandTexture)
        {
            drawList->AddImage(
                reinterpret_cast<ImTextureID>(currentBrandTexture),
                ImVec2(p1.x - 70.0f, p0.y + 10.0f),
                ImVec2(p1.x - 10.0f, p0.y + 70.0f),
                ImVec2(0, 0), ImVec2(1, 1),
                IM_COL32(255, 255, 255, 230)
            );
        }

        // Podpisy TUZ i WOM
        ImU32 hitchColor = t.isRearHitchLowered ? IM_COL32(40, 240, 80, 255) : IM_COL32(240, 180, 30, 255);
        char hitchText[64];
        snprintf(hitchText, sizeof(hitchText), "TUZ: %.1f%% [%s]", t.rearHitchPositionPct, t.isRearHitchLowered ? "W ZIEMI (PRACA)" : "PODNIESIONY");
        drawList->AddText(ImVec2(p0.x + 15.0f, p0.y + 15.0f), hitchColor, hitchText);

        char ptoText[64];
        ImU32 ptoColor = t.isRearPtoActive ? IM_COL32(40, 240, 80, 255) : IM_COL32(140, 140, 140, 255);
        snprintf(ptoText, sizeof(ptoText), "WOM: %.0f RPM [%s]", t.rearPtoRpm, t.isRearPtoActive ? "AKTYWNY" : "WYLACZONY");
        drawList->AddText(ImVec2(p0.x + 15.0f, p0.y + 35.0f), ptoColor, ptoText);

        // Kontrolka Stanu Roboczego
        ImVec2 badgePos = ImVec2(p0.x + 15.0f, p0.y + canvasH - 45.0f);
        drawList->AddRectFilled(badgePos, ImVec2(badgePos.x + 180.0f, badgePos.y + 32.0f),
                                t.isWorkConditionMet ? IM_COL32(20, 120, 40, 220) : IM_COL32(60, 60, 65, 200), 5.0f);
        drawList->AddRect(badgePos, ImVec2(badgePos.x + 180.0f, badgePos.y + 32.0f),
                          t.isWorkConditionMet ? IM_COL32(40, 240, 80, 255) : IM_COL32(130, 130, 130, 255), 5.0f, 0, 1.5f);
        drawList->AddText(ImVec2(badgePos.x + 12.0f, badgePos.y + 8.0f), IM_COL32(255, 255, 255, 255),
                          t.isWorkConditionMet ? "STAN ROBOCZY: ON" : "STAN ROBOCZY: OFF");

        // Poślizg kół
        char slipText[64];
        snprintf(slipText, sizeof(slipText), "Poslizg kol: %.1f%%", t.wheelSlipPct);
        ImU32 slipColor = (t.wheelSlipPct > 15.0f) ? IM_COL32(255, 60, 60, 255) : IM_COL32(200, 220, 240, 255);
        drawList->AddText(ImVec2(p1.x - 160.0f, p0.y + canvasH - 30.0f), slipColor, slipText);

        // Przesunięcie kursora pod podgląd ciągnika
        ImGui::Dummy(ImVec2(canvasW, canvasH + 8.0f));

        // 3. Kafelki i wskaźniki telemetryczne (Responsywna tabela zamiast kolumn z obcinaniem)
        if (ImGui::BeginTable("TractorMetersTable", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Silnik (EEC1)", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Paliwo (LFE / DD)", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("TUZ i Predkosc", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            ImGui::TableNextRow();

            // Kolumna 1: Silnik
            ImGui::TableNextColumn();
            ImGui::Text("Obroty: %.0f RPM", t.engineRpm);
            ImGui::ProgressBar(t.engineRpm / 2500.0f, ImVec2(-1, 14), "");
            ImGui::Spacing();
            ImGui::Text("Obciazenie: %.0f%%", t.engineLoadPct);
            ImGui::ProgressBar(t.engineLoadPct / 100.0f, ImVec2(-1, 14), "");
            ImGui::Spacing();
            ImGui::Text("Temp. plynu: %.1f C", t.coolantTempC);
            if (t.oilTempC > 0.0f)
            {
                ImGui::Text("Temp. oleju: %.1f C", t.oilTempC);
            }
            if (t.boostPressureKpa > 0.0f)
            {
                ImGui::Text("Turbo: %.0f kPa", t.boostPressureKpa);
            }

            // Kolumna 2: Paliwo
            ImGui::TableNextColumn();
            ImGui::Text("Chwilowe: %.1f l/h", t.fuelRateLitersPerHour);
            ImGui::Spacing();
            ImGui::Text("Srednie: %.1f l/ha", t.avgFuelEconomyLitersPerHa);
            ImGui::Spacing();
            ImGui::Text("Poziom w baku: %.0f%%", t.fuelLevelPct);
            ImGui::ProgressBar(t.fuelLevelPct / 100.0f, ImVec2(-1, 14), "");
            if (t.engineHours > 0.1f)
            {
                ImGui::Spacing();
                ImGui::Text("Motogodziny: %.1f h", t.engineHours);
            }

            // Kolumna 3: TUZ i Prędkości
            ImGui::TableNextColumn();
            ImGui::Text("Radar: %.1f km/h", t.groundSpeedKph);
            ImGui::Text("Kola:  %.1f km/h", t.wheelSpeedKph);
            ImGui::Spacing();
            ImGui::Text("TUZ Tyl: %.1f%%", t.rearHitchPositionPct);
            ImGui::ProgressBar((100.0f - t.rearHitchPositionPct) / 100.0f, ImVec2(-1, 14), t.isRearHitchLowered ? "W ZIEMI" : "PODNIESIONY");

            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Separator();

        // 4. Wykryte Moduły / Sterowniki Ciągnika (Auto-Sniffer J1939)
        if (ImGui::CollapsingHeader("Wykryte Sterowniki Ciagnika (Auto-Sniffer ECU)", ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto ecus = get_discovered_ecus();
            if (ecus.empty())
            {
                ImGui::TextDisabled("Brak wykrytych modulow na szynie ciagnika. Kliknij 'Auto-Wykryj Baudrate' powyzej!");
            }
            else
            {
                if (ImGui::BeginTable("DiscoveredEcusTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
                {
                    ImGui::TableSetupColumn("Nazwa Modulu Ciagnika", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Adres (SA)", ImGuiTableColumnFlags_WidthFixed, 85.0f);
                    ImGui::TableSetupColumn("Czestotliwosc", ImGuiTableColumnFlags_WidthFixed, 95.0f);
                    ImGui::TableSetupColumn("Liczba Ramek", ImGuiTableColumnFlags_WidthFixed, 95.0f);
                    ImGui::TableHeadersRow();

                    for (const auto &ecu : ecus)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextColored(ImVec4(0.35f, 0.9f, 1.0f, 1.0f), "%s", ecu.name.c_str());

                        ImGui::TableNextColumn();
                        ImGui::Text("0x%02X (%u)", ecu.sa, ecu.sa);

                        ImGui::TableNextColumn();
                        ImGui::Text("%.1f Hz", ecu.frameRateHz);

                        ImGui::TableNextColumn();
                        ImGui::Text("%u", ecu.frameCount);
                    }
                    ImGui::EndTable();
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();

        // 5. Dolne sterowanie progami
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Prog podnosnika (praca ponizej):");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("##HitchThresh", &hitchLoweredThresholdPct, 5.0f, 60.0f, "%.0f%%");

        // Jeśli tryb symulacji - suwaki do testu
        if (currentSource == TractorCanSource::SimulationMode)
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f), "SUWAKI SYMULACJI (TEST NA STOLE):");
            ImGui::SliderFloat("Pozycja TUZ (%)", &simHitch, 0.0f, 100.0f, "%.1f%%");
            ImGui::SliderFloat("Obroty WOM (RPM)", &simPto, 0.0f, 1100.0f, "%.0f RPM");
            ImGui::SliderFloat("Obroty Silnika", &simEngineRpm, 600.0f, 2600.0f, "%.0f RPM");
            ImGui::SliderFloat("Spalanie (l/h)", &simFuelRate, 0.0f, 60.0f, "%.1f l/h");
        }

        ImGui::End();
    }

    // =========================================================================
    // GUI: Menu Konfiguracji PGN i Sniffer
    // =========================================================================
    void TractorEcuManager::render_pgn_config_dialog(bool *p_open)
    {
        if (p_open && !*p_open) return;

        ImGui::SetNextWindowSizeConstraints(ImVec2(640, 520), ImVec2(1920, 1080));
        ImGui::SetNextWindowSize(ImVec2(740, 600), ImGuiCond_FirstUseEver);

        if (!ImGui::Begin("Konfiguracja PGN Ciagnika i Drugiego PCAN", p_open))
        {
            ImGui::End();
            return;
        }

        // 1. Wybór źródła danych CAN
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "1. Zrodlo danych szyny ciagnika (J1939 / ISOBUS):");
        int srcChoice = static_cast<int>(currentSource);
        if (ImGui::RadioButton("Dedykowany drugi PCAN-USB (np. PCAN_USBBUS2 - CAN ciagnika)", &srcChoice, 0))
        {
            set_source(TractorCanSource::SecondPcanUsb);
        }
        if (ImGui::RadioButton("Wspoldzielona magistrala z CAN 1 (Gdy ciagnik ma tylko 1 gniazdo ISOBUS)", &srcChoice, 1))
        {
            set_source(TractorCanSource::SharedWithCan1);
        }
        if (ImGui::RadioButton("Symulacja wewnetrzna (Do testow bez podlaczonego ciagnika)", &srcChoice, 2))
        {
            set_source(TractorCanSource::SimulationMode);
        }

        if (currentSource == TractorCanSource::SecondPcanUsb)
        {
            ImGui::Indent();
            ImGui::Text("Kanal PCAN 2:");
            ImGui::SameLine();
            int chIdx = (pcanChannel >= PCAN_USBBUS1 && pcanChannel <= PCAN_USBBUS8) ? (pcanChannel - PCAN_USBBUS1) : 1;
            ImGui::SetNextItemWidth(120);
            if (ImGui::Combo("##pcan2_ch", &chIdx, "PCAN-USB 1\0PCAN-USB 2\0PCAN-USB 3\0PCAN-USB 4\0PCAN-USB 5\0PCAN-USB 6\0PCAN-USB 7\0PCAN-USB 8\0\0"))
            {
                set_pcan_channel(static_cast<TPCANHandle>(PCAN_USBBUS1 + chIdx));
            }
            ImGui::SameLine();
            ImGui::Text("Baudrate:");
            ImGui::SameLine();
            int baudChoice = (pcanBaud == PCAN_BAUD_500K) ? 1 : 0;
            ImGui::SetNextItemWidth(120);
            if (ImGui::Combo("##pcan2_baud", &baudChoice, "250 kbit/s\000500 kbit/s\0\0"))
            {
                set_pcan_baudrate(baudChoice == 1 ? PCAN_BAUD_500K : PCAN_BAUD_250K);
            }
            ImGui::Unindent();
        }

        ImGui::Separator();

        // 2. Profil Ciągnika i Wybór Modelu
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "2. Profil Marki Ciagnika i Model:");
        int profChoice = static_cast<int>(currentProfile);
        const char* profNames = "Standard ISO 11783-7 / SAE J1939\0John Deere (AutoTrac / Tractor)\0Fendt Vario (K-CAN / ISOBUS)\0Case IH / New Holland (CNH)\0Claas (Cebis / Telematics)\0Deutz-Fahr / Same (SDF Agrotron)\0Profil Wlasny (Custom)\0\0";
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::Combo("##brand_profile", &profChoice, profNames))
        {
            load_brand_profile(static_cast<TractorBrandProfile>(profChoice));
        }

        ImGui::SameLine();
        ImGui::Text("Model:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::BeginCombo("##dialog_tractor_model", tractorModels[selectedModelIndex].displayName.c_str()))
        {
            for (int i = 0; i < static_cast<int>(tractorModels.size()); ++i)
            {
                bool isSelected = (selectedModelIndex == i);
                if (ImGui::Selectable(tractorModels[i].displayName.c_str(), isSelected))
                {
                    set_selected_model_index(i);
                }
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // Miniaturka ciągnika i logo marki na żywo
        if (currentBrandTexture || currentTractorTexture)
        {
            ImGui::SameLine();
            if (currentBrandTexture)
            {
                ImGui::Image(reinterpret_cast<ImTextureID>(currentBrandTexture), ImVec2(32.0f, 32.0f));
                ImGui::SameLine();
            }
            if (currentTractorTexture)
            {
                ImGui::Image(reinterpret_cast<ImTextureID>(currentTractorTexture), ImVec2(50.0f, 36.0f));
            }
        }

        ImGui::Separator();

        // 3. Tabela Mapowania PGN
        if (ImGui::CollapsingHeader("Tabela Mapowania Parametrow i PGN", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::BeginTable("PgnConfigTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
            {
                ImGui::TableSetupColumn("Parametr", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("PGN (Dec / Hex)", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                ImGui::TableSetupColumn("Bajt startowy", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                ImGui::TableSetupColumn("Dlugosc", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Skala / Mnoznik", ImGuiTableColumnFlags_WidthFixed, 85.0f);
                ImGui::TableSetupColumn("Jednostka", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < fieldConfigs.size(); ++i)
                {
                    auto &cfg = fieldConfigs[i];
                    ImGui::TableNextRow();

                    // 1. Nazwa parametru
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(cfg.name.c_str());

                    // 2. PGN (Dec / Hex)
                    ImGui::TableNextColumn();
                    ImGui::PushID(static_cast<int>(i * 20 + 4));
                    int pgnVal = static_cast<int>(cfg.pgn);
                    ImGui::SetNextItemWidth(75.0f);
                    if (ImGui::InputInt("##pgn", &pgnVal, 0, 0))
                    {
                        cfg.pgn = static_cast<uint32_t>(std::max(0, pgnVal));
                    }
                    ImGui::SameLine(0, 4);
                    ImGui::TextDisabled("0x%X", cfg.pgn);
                    ImGui::PopID();

                    // 3. Bajt startowy (D0..D7)
                    ImGui::TableNextColumn();
                    ImGui::PushID(static_cast<int>(i * 20 + 1));
                    if (ImGui::Button("-", ImVec2(24, 22)))
                    {
                        if (cfg.startByte > 0) cfg.startByte--;
                    }
                    ImGui::SameLine(0, 4);
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "D%u", cfg.startByte);
                    ImGui::SameLine(0, 4);
                    if (ImGui::Button("+", ImVec2(24, 22)))
                    {
                        if (cfg.startByte < 7) cfg.startByte++;
                    }
                    ImGui::PopID();

                    // 4. Długość bajtów (1 B .. 4 B)
                    ImGui::TableNextColumn();
                    ImGui::PushID(static_cast<int>(i * 20 + 2));
                    if (ImGui::Button("-", ImVec2(24, 22)))
                    {
                        if (cfg.lengthBytes > 1) cfg.lengthBytes--;
                    }
                    ImGui::SameLine(0, 4);
                    ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "%u B", cfg.lengthBytes);
                    ImGui::SameLine(0, 4);
                    if (ImGui::Button("+", ImVec2(24, 22)))
                    {
                        if (cfg.lengthBytes < 4) cfg.lengthBytes++;
                    }
                    ImGui::PopID();

                    // 5. Skala / Mnożnik
                    ImGui::TableNextColumn();
                    ImGui::PushID(static_cast<int>(i * 20 + 3));
                    ImGui::SetNextItemWidth(75.0f);
                    ImGui::InputFloat("##s", &cfg.scale, 0.0f, 0.0f, "%.4f");
                    ImGui::PopID();

                    // 6. Jednostka
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(cfg.unit.c_str());
                }

                ImGui::EndTable();
            }
        }

        // 4. Sniffer Ramek Ciągnika z podświetlaniem zmian (Auto-Detect)
        if (ImGui::CollapsingHeader("Live CAN Sniffer - Wykrywanie i podglad ramek z ciagnika"))
        {
            if (ImGui::Button("Wyczysc liste"))
            {
                clear_sniffer();
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.4f, 1.0f), "Wskazowka: Ruszaj wajcha TUZ lub wlacz WOM w ciagniku - zmieniajace sie bajty podswietla sie na zolto!");

            auto frames = get_sniffed_frames();
            if (ImGui::BeginTable("SnifferTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 180)))
            {
                ImGui::TableSetupColumn("PGN", ImGuiTableColumnFlags_WidthFixed, 110);
                ImGui::TableSetupColumn("SA", ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn("Licznik", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Bajty danych (D0 .. D7)", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Akcja", ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableHeadersRow();

                for (const auto &f : frames)
                {
                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    ImGui::Text("%u (0x%X)", f.pgn, f.pgn);

                    ImGui::TableNextColumn();
                    ImGui::Text("0x%02X", f.sa);

                    ImGui::TableNextColumn();
                    ImGui::Text("%u", f.count);

                    ImGui::TableNextColumn();
                    for (int b = 0; b < f.dlc; ++b)
                    {
                        if (f.byteChanged[b])
                        {
                            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.1f, 1.0f), "[%02X]", f.data[b]);
                        }
                        else
                        {
                            ImGui::Text("%02X", f.data[b]);
                        }
                        ImGui::SameLine();
                    }
                    ImGui::NewLine();

                    ImGui::TableNextColumn();
                    ImGui::PushID(static_cast<int>(f.pgn));
                    if (ImGui::SmallButton("Ustaw jako TUZ"))
                    {
                        for (auto &c : fieldConfigs)
                        {
                            if (c.name.find("TUZ Tyl") != std::string::npos)
                            {
                                c.pgn = f.pgn;
                                for (int b = 0; b < f.dlc; ++b)
                                {
                                    if (f.byteChanged[b]) { c.startByte = static_cast<uint8_t>(b); break; }
                                }
                            }
                        }
                    }
                    ImGui::PopID();
                }

                ImGui::EndTable();
            }
        }

        ImGui::End();
    }
}
