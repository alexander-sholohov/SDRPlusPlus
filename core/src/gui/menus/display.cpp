#include <gui/menus/display.h>
#include <imgui.h>
#include <gui/gui.h>
#include <core.h>
#include <gui/colormaps.h>
#include <gui/gui.h>
#include <gui/main_window.h>
#include <signal_path/signal_path.h>
#include <gui/style.h>
#include <utils/optionlist.h>
#include <algorithm>

namespace displaymenu {
    bool showWaterfall;
    bool fullWaterfallUpdate = true;
    int colorMapId = 0;
    std::vector<std::string> colorMapNames;
    std::string colorMapNamesTxt = "";
    std::string colorMapAuthor = "";
    int selectedWindow = 0;
    int fftRate = 20;
    int uiScaleId = 0;
    int transcieverLayoutId = 0;
    bool restartRequired = false;
    bool fftHold = false;
    int fftHoldSpeed = 60;
    bool fftSmoothing = false;
    int fftSmoothingSpeed = 100;
    bool smallScreen = false;

    TranscieverLayout transcieverLayout = TRAL_NONE;

#ifdef __ANDROID__
    float displayDensity = 1.0;  // 1.0 = 160 dpi. 3.5 = kinda high dpi etc. Coincides with good default scale
#endif

    OptionList<float, float> uiScales;

    const int FFTSizes[] = {
        524288,
        262144,
        131072,
        65536,
        32768,
        16384,
        8192,
        4096,
        2048,
        1024
    };

    const char* FFTSizesStr = "524288\0"
                              "262144\0"
                              "131072\0"
                              "65536\0"
                              "32768\0"
                              "16384\0"
                              "8192\0"
                              "4096\0"
                              "2048\0"
                              "1024\0";

    int fftSizeId = 0;

    const IQFrontEnd::FFTWindow fftWindowList[] = {
        IQFrontEnd::FFTWindow::RECTANGULAR,
        IQFrontEnd::FFTWindow::BLACKMAN,
        IQFrontEnd::FFTWindow::NUTTALL
    };

    void updateFFTSpeeds() {
        gui::waterfall.setFFTHoldSpeed((float)fftHoldSpeed / ((float)fftRate * 10.0f));
        gui::waterfall.setFFTSmoothingSpeed(std::min<float>((float)fftSmoothingSpeed / (float)(fftRate * 10.0f), 1.0f));
    }

    void init() {
        showWaterfall = core::configManager.conf["showWaterfall"];
        showWaterfall ? gui::waterfall.showWaterfall() : gui::waterfall.hideWaterfall();
        std::string colormapName = core::configManager.conf["colorMap"];
        if (colormaps::maps.find(colormapName) != colormaps::maps.end()) {
            colormaps::Map map = colormaps::maps[colormapName];
            gui::waterfall.updatePalletteFromArray(map.map, map.entryCount);
        }

        for (auto const& [name, map] : colormaps::maps) {
            colorMapNames.push_back(name);
            colorMapNamesTxt += name;
            colorMapNamesTxt += '\0';
            if (name == colormapName) {
                colorMapId = (colorMapNames.size() - 1);
                colorMapAuthor = map.author;
            }
        }

        fullWaterfallUpdate = core::configManager.conf["fullWaterfallUpdate"];
        gui::waterfall.setFullWaterfallUpdate(fullWaterfallUpdate);

        fftSizeId = 3;
        int fftSize = core::configManager.conf["fftSize"];
        for (int i = 0; i < 7; i++) {
            if (fftSize == FFTSizes[i]) {
                fftSizeId = i;
                break;
            }
        }
        sigpath::iqFrontEnd.setFFTSize(FFTSizes[fftSizeId]);

        fftRate = core::configManager.conf["fftRate"];
        sigpath::iqFrontEnd.setFFTRate(fftRate);

        selectedWindow = std::clamp<int>((int)core::configManager.conf["fftWindow"], 0, (sizeof(fftWindowList) / sizeof(IQFrontEnd::FFTWindow)) - 1);
        sigpath::iqFrontEnd.setFFTWindow(fftWindowList[selectedWindow]);

        gui::menu.locked = core::configManager.conf["lockMenuOrder"];

        smallScreen = core::configManager.conf["smallScreen"];
        transcieverLayout = core::configManager.conf["transcieverLayout"];

        fftHold = core::configManager.conf["fftHold"];
        fftHoldSpeed = core::configManager.conf["fftHoldSpeed"];
        gui::waterfall.setFFTHold(fftHold);
        fftSmoothing = core::configManager.conf["fftSmoothing"];
        fftSmoothingSpeed = core::configManager.conf["fftSmoothingSpeed"];
        updateFFTSpeeds();


        // Define and load UI scales

        std::vector<float> scales = {0.25f, 0.5f, 0.66f, 0.75f, 0.9f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f, 4.0f};
        bool hasNativeScale = false;
#ifdef __ANDROID__
        for (int i = 0; i < scales.size(); i++) {
            float scale = scales[i];
            if (scale == displayDensity) {
                hasNativeScale = true;
            }
        }
        if (!hasNativeScale) {
            uiScales.define(displayDensity, std::to_string((int)(displayDensity * 100)) + "% (native)", displayDensity);
        }
#endif
        for (int i = 0; i < scales.size(); i++) {
            float scale = scales[i];
            uiScales.define(scale, std::to_string((int)(scale * 100)) + "%", scale);
        }

        uiScaleId = uiScales.valueId(style::uiScale);
    }


    void draw(void* ctx) {
        float menuWidth = ImGui::GetContentRegionAvail().x;
        bool homePressed = ImGui::IsKeyPressed(ImGuiKey_Home, false);
        if (ImGui::Checkbox("Phone layout##_sdrpp", &smallScreen)) {
            core::configManager.acquire();
            core::configManager.conf["smallScreen"] = smallScreen;
            core::configManager.release(true);
        }
        if (ImGui::RadioButton("Layout: default ##_sdrpp", transcieverLayout == TRAL_NONE)) {
            core::configManager.acquire();
            transcieverLayout = TRAL_NONE;
            core::configManager.conf["transcieverLayout"] = transcieverLayout;
            core::configManager.release(true);
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("SSB trx ##_sdrpp", transcieverLayout == TRAL_SSB_FIRST)) {
            core::configManager.acquire();
            transcieverLayout = TRAL_SSB_FIRST;
            core::configManager.conf["transcieverLayout"] = transcieverLayout;
            core::configManager.release(true);
        }

        if (ImGui::Checkbox("Show Waterfall##_sdrpp", &showWaterfall) || homePressed) {
            if (homePressed) { showWaterfall = !showWaterfall; }
            showWaterfall ? gui::waterfall.showWaterfall() : gui::waterfall.hideWaterfall();
            core::configManager.acquire();
            core::configManager.conf["showWaterfall"] = showWaterfall;
            core::configManager.release(true);
        }

        if (ImGui::Checkbox("Full Waterfall Update##_sdrpp", &fullWaterfallUpdate)) {
            gui::waterfall.setFullWaterfallUpdate(fullWaterfallUpdate);
            core::configManager.acquire();
            core::configManager.conf["fullWaterfallUpdate"] = fullWaterfallUpdate;
            core::configManager.release(true);
        }

        if (ImGui::Checkbox("Lock Menu Order##_sdrpp", &gui::menu.locked)) {
            core::configManager.acquire();
            core::configManager.conf["lockMenuOrder"] = gui::menu.locked;
            core::configManager.release(true);
        }
        if (ImGui::Checkbox("FFT Hold##_sdrpp", &fftHold)) {
            gui::waterfall.setFFTHold(fftHold);
            core::configManager.acquire();
            core::configManager.conf["fftHold"] = fftHold;
            core::configManager.release(true);
        }

        if (ImGui::Checkbox("FFT Smoothing##_sdrpp", &fftSmoothing)) {
            gui::waterfall.setFFTSmoothing(fftSmoothing);
            core::configManager.acquire();
            core::configManager.conf["fftSmoothing"] = fftSmoothing;
            core::configManager.release(true);
        }

        ImGui::LeftLabel("FFT Hold Speed");
        ImGui::FillWidth();
        if (ImGui::InputInt("##sdrpp_fft_hold_speed", &fftHoldSpeed)) {
            updateFFTSpeeds();
            core::configManager.acquire();
            core::configManager.conf["fftHoldSpeed"] = fftHoldSpeed;
            core::configManager.release(true);
        }

        ImGui::LeftLabel("FFT Smoothing Speed");
        ImGui::FillWidth();
        if (ImGui::InputInt("##sdrpp_fft_smoothing_speed", &fftSmoothingSpeed)) {
            fftSmoothingSpeed = std::max<int>(fftSmoothingSpeed, 1);
            updateFFTSpeeds();
            core::configManager.acquire();
            core::configManager.conf["fftSmoothingSpeed"] = fftSmoothingSpeed;
            core::configManager.release(true);
        }

        ImGui::LeftLabel("High-DPI Scaling");
        ImGui::FillWidth();
        if (ImGui::Combo("##sdrpp_ui_scale", &uiScaleId, uiScales.txt)) {
            core::configManager.acquire();
            core::configManager.conf["uiScale"] = uiScales[uiScaleId];
            core::configManager.release(true);
            restartRequired = true;
        }

        ImGui::LeftLabel("FFT Framerate");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt("##sdrpp_fft_rate", &fftRate, 1, 10)) {
            fftRate = std::max<int>(1, fftRate);
            sigpath::iqFrontEnd.setFFTRate(fftRate);
            updateFFTSpeeds();
            core::configManager.acquire();
            core::configManager.conf["fftRate"] = fftRate;
            core::configManager.release(true);
        }

        ImGui::LeftLabel("FFT Size");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo("##sdrpp_fft_size", &fftSizeId, FFTSizesStr)) {
            sigpath::iqFrontEnd.setFFTSize(FFTSizes[fftSizeId]);
            core::configManager.acquire();
            core::configManager.conf["fftSize"] = FFTSizes[fftSizeId];
            core::configManager.release(true);
        }

        ImGui::LeftLabel("FFT Window");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo("##sdrpp_fft_window", &selectedWindow, "Rectangular\0Blackman\0Nuttall\0")) {
            sigpath::iqFrontEnd.setFFTWindow(fftWindowList[selectedWindow]);
            core::configManager.acquire();
            core::configManager.conf["fftWindow"] = selectedWindow;
            core::configManager.release(true);
        }

        if (colorMapNames.size() > 0) {
            ImGui::LeftLabel("Color Map");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::Combo("##_sdrpp_color_map_sel", &colorMapId, colorMapNamesTxt.c_str())) {
                colormaps::Map map = colormaps::maps[colorMapNames[colorMapId]];
                gui::waterfall.updatePalletteFromArray(map.map, map.entryCount);
                core::configManager.acquire();
                core::configManager.conf["colorMap"] = colorMapNames[colorMapId];
                core::configManager.release(true);
                colorMapAuthor = map.author;
            }
            ImGui::Text("Color map Author: %s", colorMapAuthor.c_str());
        }

        if (restartRequired) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Restart required.");
        }
    }
}