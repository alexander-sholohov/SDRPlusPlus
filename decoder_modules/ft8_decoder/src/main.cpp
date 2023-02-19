#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <gui/widgets/waterfall.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <filesystem>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <dsp/buffer/reshaper.h>
#include <dsp/multirate/rational_resampler.h>
#include <dsp/sink/handler_sink.h>
#include <gui/widgets/folder_select.h>
#include <gui/widgets/symbol_diagram.h>
#include <fstream>
#include <chrono>
#include "ft8_decoder.h"
#include "../../radio/src/demodulators/usb.h"
#include <utils/kmeans.h>

#include <spdlog/sinks/android_sink.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "ft8_decoder",
    /* Description:     */ "FT8 Decoder for SDR++",
    /* Author:          */ "FT8 fathers and then I added few lines",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

ConfigManager config;
CTY cty;

#define INPUT_SAMPLE_RATE 14400

enum DecodedMode {
    DM_FT8 = 1
};

static ImVec2 baseTextSize;


struct CallHashCache {

    struct CallWithTS {
        std::string call;
        int hash10;
        int hash12;
        int hash22;
        long long lastseen;
    };

    std::vector<CallWithTS> calls;

    long long lastctm = 0;

    void addCall(std::string call, long long ctm) {     // call without < .. >
        if (call.length() < 3) {        // CQ, DE etc
            return;
        }
        call = trim(call);
        if (call.find(' ') != std::string::npos) {  // CQ DX etc
            return;
        }
        if (call.find('<') != std::string::npos) {  // <1:2983> or <...>
            return;
        }
        call = normcall(call);
        for (auto &c : calls) {
            if (c.call == call) {
                c.lastseen = ctm;
                return;
            }
        }
        calls.emplace_back(CallWithTS{call, ihashcall(call, 10), ihashcall(call, 12), ihashcall(call, 22), ctm});
        if (lastctm != ctm) {
            calls.erase(std::remove_if(calls.begin(), calls.end(), [ctm](const CallWithTS &c) { return ctm - c.lastseen > 180000; }), calls.end());
            lastctm = ctm;
        }
    }

    std::string findCall(const std::string &hashed, long long ctm) { // hashed: <1:2983> or <2:2983> or <0:3498>
        if (hashed.size() == 0) {
            return "";
        }
        if (hashed.front() == '<' && hashed.back() == '>') {
            int hash = std::stoi(hashed.substr(3, hashed.size()-4));
            int mode = hashed[1] - '0';
            for (auto &c : calls) {
                if (mode == 0 && c.hash10 == hash) {
                    c.lastseen = ctm;
                    return trim(c.call);
                }
                if (mode == 1 && c.hash12 == hash) {
                    c.lastseen = ctm;
                    return trim(c.call);
                }
                if (mode == 2 && c.hash22 == hash) {
                    c.lastseen = ctm;
                    return trim(c.call);
                }
            }
        }
        return ""; // not found
    }

    std::string trim(std::string in) {
        while (in.size() > 0 && in[0] == ' ') {
            in.erase(0, 1);
        }
        while (in.size() > 0 && in[in.size()-1] == ' ') {
            in.erase(in.end() - 1);
        }
        return in;
    }

    std::string normcall(std::string call) {
        while(call.size() > 0 && call[0] == ' ')
            call.erase(0, 1);
        while(call.size() > 0 && call[call.size()-1] == ' ')
            call.erase(call.end() - 1);
        while(call.size() < 11)
            call += " ";
        return call;
    }


    // via https://github.com/rtmrtmrtmrtm/ft8mon/blob/master/unpack.cc
    int ihashcall(const std::string &call, int m)
    {
        const char *chars = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/";

        unsigned long long x = 0;
        for(int i = 0; i < 11; i++){
            int c = call[i];
            const char *p = strchr(chars, c);
            assert(p);
            int j = p - chars;
            x = 38*x + j;
        }
        x = x * 47055833459LL;
        x = x >> (64 - m);

        return x;
    }
};


struct DecodedResult {
    DecodedMode mode;
    long long decodeEndTimestamp;
    long long frequency;
    std::string shortString;
    // above is unique key

    DecodedResult(DecodedMode mode, long long int decodeEndTimestamp, long long int frequency, const std::string& shortString, const std::string& detailedString);

    bool operator == (const DecodedResult&another) const {
        return mode == another.mode && decodeEndTimestamp == another.decodeEndTimestamp && frequency == another.frequency && shortString == another.shortString;
    }

    std::string detailedString;
    double strength; // normalized 0..1.0
    double strengthRaw = 0;

    // below, zero means not layed out.
    double width = -1;
    double height = -1;
    double group = 0;
    double intensity = 0;
    double distance = 0;
    std::string qth = "";
    long long addedTime = 0;

};


struct DrawableDecodedResult {
    virtual long long getDecodeEndTimestamp() = 0;
    virtual long long getFrequency() = 0;
    double layoutY = 0;     // y on the waterfall relative to decodeEndTimestamp, in pixels;
    double layoutX = 0;     // x on the waterfall relative its to frequency (after rescale, relayout needed), in pixels
    virtual void draw(const ImVec2& origin, ImGuiWindow* pWindow) = 0;
};

struct FT8DrawableDecodedResult : DrawableDecodedResult {

    DecodedResult* result;
    FT8DrawableDecodedResult(DecodedResult* result);
    long long int getDecodeEndTimestamp() override;
    long long int getFrequency() override;
    void draw(const ImVec2& origin, ImGuiWindow* pWindow) override;
};

struct FT8DrawableDecodedDistRange : DrawableDecodedResult {
    std::string extraText;
    long long freq;
    explicit FT8DrawableDecodedDistRange(const std::string& extraText, long long freq);

    long long int getDecodeEndTimestamp() override;
    long long int getFrequency() override;
    void draw(const ImVec2& origin, ImGuiWindow* pWindow) override;
};

struct KMeansDR {
    double distance;
    int group = 0;
    int drIndex;
    double _kmc;

    KMeansDR(double distance, int drIndex) : distance(distance), drIndex(drIndex), _kmc(log10(distance)) {}

    double kmeansDistanceTo(KMeansDR *another) const {
        return abs(kmeansCoord()-another->kmeansCoord());
    }

    double kmeansCoord() const {
        return _kmc;
    }

    void setKmeansCoord(double coord) {
        _kmc = coord;
    }

};


class FT8DecoderModule : public ModuleManager::Instance {

    dsp::stream<dsp::complex_t> iqdata;
    std::atomic_bool running;

    const int VFO_SAMPLE_RATE = 24000;
    //    const int CAPTURE_SAMPLE_RATE = 12000;

public:
    int rangeContainsInclusive(double largeRangeStart, double largeRangeEnd, double smallRangeStart, double smallRangeEnd) {
        if (largeRangeStart <= smallRangeStart && largeRangeEnd >= smallRangeEnd) {
            return 1;
        }
        return 0;
    }

    const int INVALID_OFFSET = 0x7FFFFFFF;
    const int USB_BANDWIDTH = 3000;

    std::vector<DecodedResult>  decodedResults;
    std::vector<std::shared_ptr<DrawableDecodedResult>>  decodedResultsDrawables;
    std::mutex decodedResultsLock;


    void addDecodedResult(const DecodedResult&x) {
        std::lock_guard g(decodedResultsLock);
        for(const auto & decodedResult : decodedResults) {
            if(decodedResult == x) {
                return;
            }
        }
        decodedResults.push_back(x);
        decodedResults.back().addedTime = (currentTimeMillis() / 250) * 250;
        decodedResultsDrawables.clear();
    }

    void clearDecodedResults() {
        std::lock_guard g(decodedResultsLock);
        decodedResults.clear();
        decodedResultsDrawables.clear();
    }


    void drawDecodedResults(const ImGui::WaterFall::WaterfallDrawArgs &args) {

        auto ctm = currentTimeMillis();
        decodedResultsLock.lock();
        auto wfHeight = args.wfMax.y - args.wfMin.y;
        auto wfWidth = args.wfMax.x - args.wfMin.x;
        //        auto timeDisplayed = (wfMax.y - wfMin.y) * sigpath::iqFrontEnd.getFFTRate() / waterfallHeight;
        double timePerLine = 1000.0 / sigpath::iqFrontEnd.getFFTRate();
        auto currentTime = sigpath::iqFrontEnd.getCurrentStreamTime();

        const ImVec2 vec2 = ImGui::GetMousePos();


        bool mustLayout = false;
        // delete obsolete ones, and detect relayout
        for(int i=0; i<decodedResults.size(); i++) {
            auto& result = decodedResults[i];
            auto resultTimeDelta = currentTime - result.decodeEndTimestamp;
            if (resultTimeDelta > 2 * 60 * 1000 + 5000) {
                decodedResults.erase(decodedResults.begin() + i);
                decodedResultsDrawables.clear();
                i--;
                continue;
            }
        }
        if (baseTextSize.y == 0) {
            ImGui::PushFont(style::baseFont);
            baseTextSize = ImGui::CalcTextSize("WW6WWW");
            ImGui::PopFont();
        }
        ImGui::PushFont(style::tinyFont);
        auto modeSize = ImGui::CalcTextSize("ft8");
        ImGui::PopFont();


        if (decodedResults.size() > 0 && decodedResultsDrawables.size() == 0) {

            KMeans<KMeansDR> kmeans;
            std::vector<KMeansDR> kmeansData;
            for(int i=0; i<decodedResults.size(); i++) {
                double dst = decodedResults[i].distance;
                if (dst <= 0) {
                    dst = 1;
                }
                kmeansData.emplace_back(KMeansDR(dst, i));
            }
            const int MAXGROUPS = 8;
            int ngroups = std::min<int>(decodedResults.size(), MAXGROUPS);
            kmeans.lloyd(kmeansData.data(), kmeansData.size(), ngroups, 50);
            std::vector<int> groupDists;
            std::vector<bool> foundGroups;
            std::vector<int> groupSortable;
            groupDists.resize(ngroups);
            groupSortable.resize(ngroups);
            foundGroups.resize(ngroups, false);
            for(int i=0; i<decodedResults.size(); i++) {
                decodedResults[i].group = kmeansData[i].group;
                groupDists[decodedResults[i].group] = decodedResults[i].distance;
                foundGroups[decodedResults[i].group] = true;
            }
            int fgroups = 0;
            for(auto fg: foundGroups) {
                if (fg) {
                    fgroups++;
                }
            }
//            spdlog::info("Found {} groups among {} results", fgroups, decodedResults.size());

            // sorting groups by distance
            for(int i=0; i<ngroups; i++) {
                groupSortable[i] = i;
            }
            std::sort(groupSortable.begin(), groupSortable.end(), [&groupDists](int a, int b) {
                return groupDists[a] > groupDists[b];
            });
            double scanY = 0;
            double scanX = 0;
            double maxColumnWidth = baseTextSize.x * 3;
            int nresults = 0;

            for(int i=0;i<ngroups; i++) {
                std::vector<DecodedResult*>insideGroup;
                for(auto & decodedResult : decodedResults) {
                    if (decodedResult.group == groupSortable[i]) {
                        insideGroup.push_back(&decodedResult);
                    }
                }
                std::sort(insideGroup.begin(), insideGroup.end(), [](DecodedResult *a, DecodedResult *b) {
                    return strcmp(a->shortString.c_str(), b->shortString.c_str()) < 0;
                });
                std::string lastAdded;
                double maxDistance = 0;
                for(auto & decodedResult : insideGroup) {
                    if (decodedResult->shortString != lastAdded) {
                        maxDistance = std::max<double>(maxDistance, decodedResult->distance);
                        lastAdded = decodedResult->shortString;
                        ImGui::PushFont(style::baseFont);
                        auto tisTextSize = ImGui::CalcTextSize(decodedResult->shortString.c_str());
                        ImGui::PopFont();
                        tisTextSize.x += modeSize.x;
                        if (scanX + tisTextSize.x > maxColumnWidth) {
                            scanX = 0;
                            scanY += baseTextSize.y;
                        }

                        auto fdr = std::make_shared<FT8DrawableDecodedResult>(decodedResult);
                        fdr->layoutX = scanX;
                        fdr->layoutY = scanY;
                        fdr->result->intensity = (1.0 / (MAXGROUPS - 1)) * i;
                        decodedResultsDrawables.emplace_back(fdr);
                        nresults++;

                        scanX += tisTextSize.x;
                    }

                }
                if (!insideGroup.empty()) {
                    auto freqq = (double)(*insideGroup.begin())->frequency;
                    auto label = "<= " + std::to_string((int)maxDistance) + " KM";
                    if (!myPos.isValid()) {
                        label = "=> setup your GRID SQUARE";
                    }
                    auto fdr2 = std::make_shared<FT8DrawableDecodedDistRange>(label, freqq);
                    fdr2->layoutX = scanX;
                    fdr2->layoutY = scanY;
//                    spdlog::info("closing: {} {} {}", i, fdr2->layoutX, fdr2->layoutY);
                    decodedResultsDrawables.emplace_back(fdr2);
                    scanX = 0;
                    scanY += baseTextSize.y * 2;
                }

            }
            totalFT8Displayed = nresults;

        }



        std::vector<ImRect> rects;
        // place new ones
        for(int i=0; i<decodedResultsDrawables.size(); i++) {
            auto& result = decodedResultsDrawables[i];

            auto df = result->getFrequency() - gui::waterfall.getCenterFrequency();
            auto dx = (wfWidth / 2) +  df / (gui::waterfall.getViewBandwidth() / 2) *  (wfWidth / 2); // in pixels, on waterfall

            auto drawX = dx;
            auto drawY = 20;
            const ImVec2 origin = ImVec2(args.wfMin.x + drawX, args.wfMin.y + drawY);

            result->draw(origin, args.window);




            //            auto resultTimeDelta = currentTime - result->getDecodeEndTimestamp();
            //            auto resultBaselineY = resultTimeDelta / timePerLine;
            //            ImGui::PushFont(style::baseFont);
            //            auto textSize = ImGui::CalcTextSize(result.shortString.c_str());
            //            ImGui::PopFont();
            //            if (textSize.x < baseTextSize.x) {
            //                textSize.x = baseTextSize.x;
            //            }
            //            textSize.y += 2;
            //            if (result.width == -1 && result.addedTime > ctm - 250) {   // dont do too often
            //                // not layed out. find its absolute Y on the waterfall
            //                for(int step = 0; step < 50; step++) {
            //                    int dy = step * textSize.y;
            //                    auto testRect = ImRect(dx, resultBaselineY + dy, dx + textSize.x, resultBaselineY + dy + textSize.y);
            //                    bool overlaps = false;
            //                    for(const auto &r : rects) {
            //                        if (r.Overlaps(testRect)) {
            //                            overlaps = true;
            //                            break;
            //                        }
            //                    }
            //                    if (!overlaps) {
            //                        result.layoutX = 0;
            //                        result.layoutY = dy;
            //                        result.width = textSize.x;
            //                        result.height = textSize.y;
            //                        break;
            //                    }
            //                }
            //                if (result.width == -1) {
            //                    result.shortString = ""; // no luck.
            //                    continue;
            //                }
            //            }
            //            if (result.width != -1) {
            //                auto drawX = dx + result.layoutX;
            //                auto drawY = resultBaselineY + result.layoutY;
            //                ImU32 white = IM_COL32(255, 255, 255, 255);
            //                ImU32 black = IM_COL32(0, 0, 0, 255);
            //                const ImVec2 origin = ImVec2(wfMin.x + drawX, wfMin.y + drawY);
            //                const char* str = result.shortString.c_str();
            //
            //                auto toColor = [](double coss) {
            //                        if (coss < 0) {
            //                            return 0;
            //                        }
            //                        return (int)(coss * 255);
            //                    };
            //                // p = 0..1
            //#ifndef M_PI
            //#define M_PI 3.14159265358979323846
            //#endif
            //                auto phase = result.intensity * 2 * M_PI;
            //                auto RR= toColor(cos((phase - M_PI / 4 - M_PI - M_PI / 4) / 2));
            //                auto GG= toColor(cos((phase - M_PI / 4 - M_PI / 2) / 2));
            //                auto BB = toColor(cos(phase - M_PI / 4));
            //
            //
            //
            //                const ImRect& drect = ImRect(drawX, drawY, drawX + textSize.x, drawY + textSize.y);
            //                window->DrawList->AddRectFilled(origin - ImVec2(modeSize.x, 0), origin + ImVec2(0, modeSize.y), IM_COL32(255, 0, 0, 160));
            //                ImGui::PushFont(style::tinyFont);
            //                window->DrawList->AddText(origin - ImVec2(modeSize.x, 0), white, "ft8");
            //                ImGui::PopFont();
            //                window->DrawList->AddRectFilled(origin, origin + textSize - ImVec2(0, 2), IM_COL32(RR, GG, BB, 160));
            //                ImGui::PushFont(style::baseFont);
            //                window->DrawList->AddText(origin + ImVec2(-1, -1), black, str);
            //                window->DrawList->AddText(origin + ImVec2(-1, +1), black, str);
            //                window->DrawList->AddText(origin + ImVec2(+1, -1), black, str);
            //                window->DrawList->AddText(origin + ImVec2(+1, +1), black, str);
            //                window->DrawList->AddText(origin, white, str);
            //                ImGui::PopFont();
            //                float nsteps = 12.0;
            //                float h = 4.0;
            //                float yy = textSize.y - h;
            //                for(float x=0; x<baseTextSize.x; x+=baseTextSize.x/nsteps) {
            //                    auto x0 = x;
            //                    auto x1 = x + baseTextSize.x/nsteps;
            //                    auto x0m = x0 + (0.8)*(x1-x0);
            //                    bool green = (x / baseTextSize.x) < result.strength;
            //                    window->DrawList->AddRectFilled(ImVec2(x0, yy) + origin, ImVec2(x0m, yy+h) + origin, green ? IM_COL32(64, 255, 0, 255) : IM_COL32(180, 180, 180, 255));
            //                    window->DrawList->AddRectFilled(ImVec2(x0m, yy) + origin, ImVec2(x1, yy+h) + origin, IM_COL32(180, 180, 180, 255));
            //                }
            ////                window->DrawList->AddRectFilled(origin, white, str);
            //                rects.emplace_back(drect);
            //            }
        }

        decodedResultsLock.unlock();
    }


    std::pair<int,int>  calculateVFOCenterOffset(double centerFrequency, double ifBandwidth) {
        int rangeStart = centerFrequency - ifBandwidth;
        int rangeEnd = centerFrequency + ifBandwidth;
        const int frequencies[] = { 1840000, 3573000, 7074000, 10136000, 14074000, 18100000, 21074000, 24915000, 28074000, 50000000 };
        for (auto q = std::begin(frequencies); q != std::end(frequencies); q++) {
            auto center = (*q + USB_BANDWIDTH);
            if (rangeContainsInclusive(rangeStart, rangeEnd, (double)(center - USB_BANDWIDTH), (double(center + USB_BANDWIDTH)))) {
                return std::make_pair(center - centerFrequency, center);
            }
        }
        return std::make_pair(INVALID_OFFSET, INVALID_OFFSET);
    }

    EventHandler<double> iqSampleRateListener;
    EventHandler<ImGui::WaterFall::WaterfallDrawArgs> afterWaterfallDrawListener;
    EventHandler<bool> onPlayStateChange;

    FT8DecoderModule(std::string name) {
        this->name = name;
        gui::waterfall.afterWaterfallDraw.bindHandler(&afterWaterfallDrawListener);
        afterWaterfallDrawListener.ctx = this;
        afterWaterfallDrawListener.handler = [](ImGui::WaterFall::WaterfallDrawArgs args, void* ctx) {
            ((FT8DecoderModule*)ctx)->drawDecodedResults(args);
        };
        gui::mainWindow.onPlayStateChange.bindHandler(&onPlayStateChange);
        onPlayStateChange.ctx = this;
        onPlayStateChange.handler = [](bool playing, void* ctx) {
            ((FT8DecoderModule*)ctx)->clearDecodedResults();
        };
        decodedResults.reserve(2000);  // to keep addresses constant.

        //        mshv_init();

        // Load config
        config.acquire();
        if (config.conf[name].find("myGrid") != config.conf[name].end()) {
            auto qq = config.conf[name]["myGrid"].get<std::string>();
            strcpy(myGrid, qq.data());
            calculateMyLoc();
        } else {
            myGrid[0] = 0;
        }
        if (config.conf[name].find("myCallsign") != config.conf[name].end()) {
            auto qq = config.conf[name]["myCallsign"].get<std::string>();
            strcpy(myCallsign, qq.data());
        } else {
            myCallsign[0] = 0;
        }
        if (config.conf[name].find("processingEnabledFT8") != config.conf[name].end()) {
            processingEnabledFT8 = config.conf[name]["processingEnabledFT8"].get<bool>();
        }
        if (config.conf[name].find("processingEnabledFT4") != config.conf[name].end()) {
            processingEnabledFT4 = config.conf[name]["processingEnabledFT4"].get<bool>();
        }
        if (config.conf[name].find("enablePSKReporter") != config.conf[name].end()) {
            enablePSKReporter = config.conf[name]["enablePSKReporter"].get<bool>();
        }
        config.release(true);
        //        if (!config.conf.contains(name)) {
        //            config.conf[name]["showLines"] = false;
        //        }
        //        showLines = config.conf[name]["showLines"];
        //        if (showLines) {
        //            diag.lines.push_back(-0.75f);
        //            diag.lines.push_back(-0.25f);
        //            diag.lines.push_back(0.25f);
        //            diag.lines.push_back(0.75f);
        //        }
        config.release(true);

        running = true;

        //        // Initialize DSP here
        //        decoder.init(vfo->output, INPUT_SAMPLE_RATE, lsfHandler, this);
        //        resamp.init(decoder.out, 8000, audioSampRate);
        //        reshape.init(decoder.diagOut, 480, 0);
        //        diagHandler.init(&reshape.out, _diagHandler, this);
        //
        //        // Start DSO Here
        //        decoder.start();
        //        resamp.start();
        //        reshape.start();
        //        diagHandler.start();
        //
        //        // Setup audio stream
        //        srChangeHandler.ctx = this;
        //        srChangeHandler.handler = sampleRateChangeHandler;
        //        stream.init(&srChangeHandler, audioSampRate);
        //        sigpath::sinkManager.registerStream(name, &stream);
        //
        //        stream.start();

        gui::menu.registerEntry(name, menuHandler, this, this);

        vfo = new dsp::channel::RxVFO(&iqdata, sigpath::iqFrontEnd.getEffectiveSamplerate(), VFO_SAMPLE_RATE, USB_BANDWIDTH, vfoOffset);

        sigpath::iqFrontEnd.onEffectiveSampleRateChange.bindHandler(&iqSampleRateListener);
        iqSampleRateListener.ctx = this;
        iqSampleRateListener.handler = [](double newSampleRate, void* ctx) {
            spdlog::info("FT8 decoder: effective sample rate changed to {}", newSampleRate);
            ((FT8DecoderModule*)ctx)->vfo->setInSamplerate(newSampleRate);
        };

        usbDemod = std::make_shared<demod::USB>();
        usbDemodConfig.acquire();
        usbDemodConfig.disableAutoSave();
        usbDemodConfig.conf[name]["USB"]["agcAttack"] = 0.0;
        usbDemodConfig.conf[name]["USB"]["agcDecay"] = 0.0;
        usbDemodConfig.release(true);
        usbDemod->init(name, &usbDemodConfig, ifChain.out, USB_BANDWIDTH, VFO_SAMPLE_RATE);
        //        usbDemod->setFrozen(true);
        ifChain.setInput(&vfo->out, [&](auto ifchainOut) {
            usbDemod->setInput(ifchainOut);
            spdlog::info("ifchain change out");
            // next
        });


        std::thread reader([&]() {
            auto _this = this;
            std::vector<dsp::stereo_t> reader;
            double ADJUST_PERIOD = 0.1; // seconds before adjusting the vfo offset after user changed the center freq
            int beforeAdjust = (int)(VFO_SAMPLE_RATE * ADJUST_PERIOD);
            int previousCenterOffset = 0;
            while (running.load()) {
                int rd = usbDemod->getOutput()->read();
                if (rd < 0) {
                    break;
                }
                beforeAdjust -= rd;
                if (beforeAdjust <= 0) {
                    beforeAdjust = (int)(VFO_SAMPLE_RATE * ADJUST_PERIOD);
                    auto [newOffset, centerOffset] = calculateVFOCenterOffset(gui::waterfall.getCenterFrequency(), gui::waterfall.getBandwidth());
                    if (centerOffset != previousCenterOffset) {
                        clearDecodedResults();
                        previousCenterOffset = centerOffset;
                    }
                    if (newOffset == INVALID_OFFSET) {
                        onTheFrequency = false;
                    }
                    else {
                        onTheFrequency = true;
                        if (newOffset == (int)vfoOffset) {
                            // do nothing
                        }
                        else {
                            vfoOffset = newOffset;
                            spdlog::info("FT8 vfo: center offset {}, bandwidth: {}", vfoOffset, USB_BANDWIDTH);
                            vfo->setOffset(vfoOffset - USB_BANDWIDTH / 2);
                        }
                    }
                }

                if (enabled && onTheFrequency) {
                    reader.resize(rd);
                    std::copy(usbDemod->getOutput()->readBuf, usbDemod->getOutput()->readBuf + rd, reader.begin());
                    handleIFData(reader);
                }
                usbDemod->getOutput()->flush();
            }
        });
        reader.detach();

        enable();
    }

    dsp::chain<dsp::complex_t> ifChain;
    dsp::channel::RxVFO* vfo;
    ConfigManager usbDemodConfig;

    long long prevBlockNumber = 0;
    std::shared_ptr<std::vector<dsp::stereo_t>> fullBlock;
    std::mutex processingBlockMutex;
    double vfoOffset = 0.0;
    std::atomic_int blockProcessorsRunning = 0;
    std::atomic_int lastFT8DecodeTime0 = 0;
    std::atomic_int lastFT8DecodeTime = 0;
    std::atomic_int  lastFT8DecodeCount = 0;
    std::atomic_int  totalFT8Displayed = 0;

    void handleIFData(const std::vector<dsp::stereo_t>& data) {
        long long int curtime = sigpath::iqFrontEnd.getCurrentStreamTime();
        long long blockNumber = (curtime / 1000) / 15;
        long long blockTime = (curtime / 1000) % 15;
        if (blockNumber != prevBlockNumber) {
            if (fullBlock) {
                bool shouldStartProcessing = false;
                std::shared_ptr<std::vector<dsp::stereo_t>> processingBlock;
                processingBlock = fullBlock;
                shouldStartProcessing = true;
                if (blockProcessorsRunning > 0) {
                    shouldStartProcessing = false;
                }
                if (shouldStartProcessing) {
                    // no processing is done.
                    if (processingBlock->size() / VFO_SAMPLE_RATE > 16 || processingBlock->size() / VFO_SAMPLE_RATE <= 13) {
                        spdlog::info("Block size is not matching: {}, curtime={}",
                                     processingBlock->size() / VFO_SAMPLE_RATE,
                                     curtime);
                        processingBlock.reset(); // clear for new one
                    }
                    else {
                        // block size is ok.
                        startBlockProcessing(processingBlock, prevBlockNumber, vfoOffset + gui::waterfall.getCenterFrequency());
                    }
                }
                fullBlock.reset();
            }
            prevBlockNumber = blockNumber;
        }
        if (!fullBlock) {
            fullBlock = std::make_shared<std::vector<dsp::stereo_t>>();
            fullBlock->reserve(16 * VFO_SAMPLE_RATE);
        }
        fullBlock->insert(std::end(*fullBlock), std::begin(data), std::end(data));
        //        spdlog::info("{} Got {} samples: {}", blockNumber, data.size(), data[0].l);
    }

    std::shared_ptr<demod::USB> usbDemod;

    CallHashCache callHashCache;
    std::mutex callHashCacheMutex;

    void startBlockProcessing(const std::shared_ptr<std::vector<dsp::stereo_t>>& block, int blockNumber, int originalOffset) {
        blockProcessorsRunning.fetch_add(1);
        std::thread processor([=]() {
            std::time_t bst = blockNumber * 15;
            spdlog::info("Start processing block, size={}, block time: {}", block->size(), std::asctime(std::gmtime(&bst)));

            int count = 0;
            long long time0 = 0;
            auto handler = [&](int mode, std::vector<std::string> result) {
                if (time0 == 0) {
                    time0 = currentTimeMillis();
                }
                auto message = result[4];
                auto pipe = message.find('|');
                std::string callsigns;
                std::string callsign;
                if (pipe != std::string::npos) {
                    callsigns = message.substr(pipe + 1);
                    message = message.substr(0, pipe);
                    std::vector<std::string> callsignsV;
                    splitStringV(callsigns, ";", callsignsV);
                    if (callsignsV.size() > 1) {
                        callsign = callsignsV[1];
                        callHashCacheMutex.lock();
                        callHashCache.addCall(callsignsV[0], bst * 1000);
                        callHashCache.addCall(callsignsV[1], bst * 1000);
                        callHashCacheMutex.unlock();

                    }
                    if (!callsign.empty() && callsign[0] == '<') {
                        callHashCacheMutex.lock();
                        auto ncallsign = callHashCache.findCall(callsign, bst * 1000);
                        spdlog::info("Found call: {} -> {}", callsign, ncallsign);
                        callsign = ncallsign;
                        callHashCacheMutex.unlock();
                    }
                } else {
                    callsign = extractCallsignFromFT8(message);
                }
                count++;
                if (callsign.empty() || callsign.find("<") != std::string::npos) {  // ignore <..> callsigns
                    return;
                }
                double distance = 0;
                CTY::Callsign cs;
                if (callsign.empty()) {
                    callsign = "?? " + message;
                } else {
                    cs = cty.findCallsign(callsign);
                    if (myPos.isValid()) {
                        auto bd = bearingDistance(myPos, cs.ll);
                        distance = bd.distance;
                    }
                }
                DecodedResult decodedResult(DM_FT8, ((long long)blockNumber) * 15 * 1000 + 15000, originalOffset, callsign, message);
                decodedResult.distance = (int)distance;
                auto strength = atof(result[1].c_str());
                strength = (strength + 24) / (24 + 24);
                if (strength < 0.0) strength = 0.0;
                if (strength > 1.0) strength = 1.0;
                decodedResult.strength = strength;
                decodedResult.strengthRaw = atof(result[1].c_str());
                decodedResult.intensity = (random() % 100) / 100.0;
                if (!cs.dxccname.empty()) {
                    decodedResult.qth = cs.dxccname;
                }
                addDecodedResult(decodedResult);
                return;
            };
            std::thread t0([&]() {
                auto start = currentTimeMillis();
                dsp::ft8::decodeFT8(VFO_SAMPLE_RATE, block->data(), block->size(), handler);
                auto end = currentTimeMillis();
                spdlog::info("FT8 decoding took {} ms", end - start);
                lastFT8DecodeCount = (int)count;
                lastFT8DecodeTime = (int)(end - start);
                lastFT8DecodeTime0 = (int)(time0 - start);
            });
            //            std::thread t1([=]() {
            //                dsp::ft8::decodeFT8(CAPTURE_SAMPLE_RATE, block->data() + CAPTURE_SAMPLE_RATE, block->size() - CAPTURE_SAMPLE_RATE, handler);
            //            });
            //            std::thread t2([=]() {
            //                dsp::ft8::decodeFT8(CAPTURE_SAMPLE_RATE, block->data() + 2 * CAPTURE_SAMPLE_RATE, block->size() - 2 * CAPTURE_SAMPLE_RATE, handler);
            //            });
            //            std::thread t3([=]() {
            //                dsp::ft8::decodeFT8(CAPTURE_SAMPLE_RATE, block->data() + 3 * CAPTURE_SAMPLE_RATE, block->size() - 3 * CAPTURE_SAMPLE_RATE, handler);
            //            });
            //            t3.join();
            //            t2.join();
            //            t1.join();
            t0.join();

            blockProcessorsRunning.fetch_add(-1);
        });
        processor.detach();
    }

    bool onTheFrequency = false;

    ~FT8DecoderModule() {
        gui::waterfall.afterWaterfallDraw.unbindHandler(&afterWaterfallDrawListener);
        sigpath::iqFrontEnd.onEffectiveSampleRateChange.unbindHandler(&iqSampleRateListener);
        gui::mainWindow.onPlayStateChange.unbindHandler(&onPlayStateChange);
        ifChain.out->stopReader();
        running = false;

        gui::menu.removeEntry(name);
        // Stop DSP Here
        //        stream.stop();
        if (enabled) {
            //            decoder.stop();
            //            resamp.stop();
            //            reshape.stop();
            //            diagHandler.stop();
            //            sigpath::vfoManager.deleteVFO(vfo);
        }

        //        sigpath::sinkManager.unregisterStream(name);
    }

    void postInit() {}

    void enable() {
        if (!enabled) {
            sigpath::iqFrontEnd.bindIQStream(&iqdata);
            vfo->start();
            ifChain.start();
            usbDemod->start();
            enabled = true;
            spdlog::info("FT8 Decoder enabled");
        }
    }

    void disable() {
        // Stop DSP here
        //        decoder.stop();
        //        resamp.stop();
        //        reshape.stop();
        //        diagHandler.stop();
        //
        //        sigpath::vfoManager.deleteVFO(vfo);

        if (enabled) {
            usbDemod->stop();
            ifChain.stop();
            vfo->stop();
            sigpath::iqFrontEnd.unbindIQStream(&iqdata);
            enabled = false;
            spdlog::info("FT8 Decoder disabled");
        }
    }

    bool isEnabled() {
        return enabled;
    }

private:
    static void menuHandler(void* ctx) {
        FT8DecoderModule* _this = (FT8DecoderModule*)ctx;
        ImGui::LeftLabel("My Grid");
        ImGui::FillWidth();
        if (ImGui::InputText(CONCAT("##_my_grid_", _this->name), _this->myGrid, 8)) {
            config.acquire();
            config.conf[_this->name]["myGrid"] = _this->myGrid;
            config.release(true);
            _this->calculateMyLoc();
        }
        ImGui::LeftLabel("My Loc: ");
        ImGui::FillWidth();
        if (_this->myPos.isValid()) {
            ImGui::Text("%+02.5f %+02.5f", _this->myPos.lat, _this->myPos.lon);
        } else {
            ImGui::Text("Invalid");
        }
        ImGui::LeftLabel("My Callsign");
        ImGui::FillWidth();
        if (ImGui::InputText(CONCAT("##_my_callsign_", _this->name), _this->myCallsign, 12)) {
            config.acquire();
            config.conf[_this->name]["myCallsign"] = _this->myCallsign;
            config.release(true);
        }
        if (ImGui::SliderInt("Layout width", &_this->layoutWidth, 3, 10, (FormatString)elements[i+4].i, elements[i+5].i)) {
            SET_DIFF_INT(elements[i].str, elements[i+1].i);
        }

        ImGui::LeftLabel("Decode FT8");
        if (ImGui::Checkbox(CONCAT("##_processing_enabled_ft8_", _this->name), &_this->processingEnabledFT8)) {
            config.acquire();
            config.conf[_this->name]["processingEnabledFT8"] = _this->processingEnabledFT8;
            config.release(true);
        }
        ImGui::SameLine();
        ImGui::Text("Count: %d(%d) in %d..%d msec", _this->lastFT8DecodeCount.load(), _this->totalFT8Displayed.load(), _this->lastFT8DecodeTime0.load(), _this->lastFT8DecodeTime.load());
        ImGui::LeftLabel("Decode FT4");
        ImGui::FillWidth();
        if (ImGui::Checkbox(CONCAT("##_processing_enabled_ft4_", _this->name), &_this->processingEnabledFT4)) {
            config.acquire();
            config.conf[_this->name]["processingEnabledFT4"] = _this->processingEnabledFT4;
            config.release(true);
        }
        ImGui::LeftLabel("PskReporter");
        ImGui::FillWidth();
        if (ImGui::Checkbox(CONCAT("##_enable_psk_reporter_", _this->name), &_this->enablePSKReporter)) {
            config.acquire();
            config.conf[_this->name]["enablePSKReporter"] = _this->enablePSKReporter;
            config.release(true);
        }
    }

    void calculateMyLoc() {
        auto ll = gridToLatLng(myGrid);
        if (ll.isValid()) {
            myPos = ll;
        }
    }

    std::string name;
    bool enabled = false;
    char myGrid[10];
    char myCallsign[13];
    LatLng myPos = LatLng::invalid();
    bool processingEnabledFT8 = true;
    bool processingEnabledFT4 = true;
    bool enablePSKReporter = true;
    int layoutWidth = 3;


    //    dsp::buffer::Reshaper<float> reshape;
    //    dsp::sink::Handler<float> diagHandler;
    //    dsp::multirate::RationalResampler<dsp::stereo_t> resamp;
    //

    std::chrono::time_point<std::chrono::high_resolution_clock> lastUpdated;
};


DecodedResult::DecodedResult(DecodedMode mode, long long int decodeEndTimestamp, long long int frequency, const std::string& shortString, const std::string& detailedString)
    : mode(mode), decodeEndTimestamp(decodeEndTimestamp), frequency(frequency), shortString(shortString), detailedString(detailedString) {

}
long long int FT8DrawableDecodedResult::getDecodeEndTimestamp() {
    return result->decodeEndTimestamp;
}
long long int FT8DrawableDecodedResult::getFrequency() {
    return result->frequency;
}

static int toColor(double coss) {
    if (coss < 0) {
        return 0;
    }
    return (int)(coss * 255);
}

void FT8DrawableDecodedResult::draw(const ImVec2& _origin, ImGuiWindow* window) {

    ImVec2 origin = _origin;
    origin += ImVec2(layoutX, layoutY);

    auto phase = result->intensity * 2 * M_PI;
    auto RR= toColor(cos((phase - M_PI / 4 - M_PI - M_PI / 4) / 2));
    auto GG= toColor(cos((phase - M_PI / 4 - M_PI / 2) / 2));
    auto BB = toColor(cos(phase - M_PI / 4));

    ImGui::PushFont(style::tinyFont);
    auto modeSize = ImGui::CalcTextSize("ft8");
    ImGui::PopFont();
    ImGui::PushFont(style::baseFont);
    const char* str = result->shortString.c_str();
    auto textSize = ImGui::CalcTextSize(str);
    ImGui::PopFont();

    ImU32 white = IM_COL32(255, 255, 255, 255);
    ImU32 black = IM_COL32(0, 0, 0, 255);

    //        const ImRect& drect = ImRect(layoutX, layoutY, layoutX + textSize.x, layoutY + textSize.y);
    window->DrawList->AddRectFilled(origin, origin + ImVec2(modeSize.x, modeSize.y), IM_COL32(255, 0, 0, 160));
    ImGui::PushFont(style::tinyFont);
    window->DrawList->AddText(origin, white, "ft8");
    ImGui::PopFont();

    origin.x += modeSize.x;

    auto maxCorner = origin + textSize - ImVec2(0, 2);
    window->DrawList->AddRectFilled(origin, maxCorner, IM_COL32(RR, GG, BB, 160));

    if (ImGui::IsMouseHoveringRect(origin, maxCorner)) {
        char buf[128];
        ImGui::BeginTooltip();

        ImGui::Text("Distance: %0.0f km", result->distance);
        ImGui::Separator();
        ImGui::Text("Strength: %0.0f dB", result->strengthRaw);
        ImGui::Separator();
        ImGui::Text("QTH: %s", result->qth.c_str());
        ImGui::Separator();
        ImGui::Text("Message: %s", result->detailedString.c_str());

        ImGui::EndTooltip();
    }


    ImGui::PushFont(style::baseFont);
    window->DrawList->AddText(origin + ImVec2(-1, -1), black, str);
    window->DrawList->AddText(origin + ImVec2(-1, +1), black, str);
    window->DrawList->AddText(origin + ImVec2(+1, -1), black, str);
    window->DrawList->AddText(origin + ImVec2(+1, +1), black, str);
    window->DrawList->AddText(origin, white, str);
    ImGui::PopFont();
    float nsteps = 12.0;
    float h = 4.0;
    float yy = textSize.y - h;
    for(float x=0; x<baseTextSize.x; x+=baseTextSize.x/nsteps) {
        auto x0 = x;
        auto x1 = x + baseTextSize.x/nsteps;
        auto x0m = x0 + (0.8)*(x1-x0);
        bool green = (x / baseTextSize.x) < result->strength;
        window->DrawList->AddRectFilled(ImVec2(x0, yy) + origin, ImVec2(x0m, yy+h) + origin, green ? IM_COL32(64, 255, 0, 255) : IM_COL32(180, 180, 180, 255));
        window->DrawList->AddRectFilled(ImVec2(x0m, yy) + origin, ImVec2(x1, yy+h) + origin, IM_COL32(180, 180, 180, 255));
    }

}
FT8DrawableDecodedResult::FT8DrawableDecodedResult(DecodedResult* result) : result(result) {}

FT8DrawableDecodedDistRange::FT8DrawableDecodedDistRange(const std::string& extraText, long long freq) : extraText(extraText), freq(freq) {

}
long long int FT8DrawableDecodedDistRange::getDecodeEndTimestamp() {
    return 0;
}
long long int FT8DrawableDecodedDistRange::getFrequency() {
    return freq;
}
void FT8DrawableDecodedDistRange::draw(const ImVec2& _origin, ImGuiWindow* window) {
    ImVec2 origin = _origin;
    origin += ImVec2(layoutX, layoutY);
    ImGui::PushFont(style::tinyFont);
    const char* str = this->extraText.c_str();
    auto strSize = ImGui::CalcTextSize(str);
    ImU32 white = IM_COL32(255, 255, 255, 255);
    window->DrawList->AddRectFilled(origin, origin + strSize, IM_COL32(0, 0, 0, 255));
    window->DrawList->AddText(origin, white, str);
    ImGui::PopFont();
}


MOD_EXPORT void _INIT_() {
    // Create default recording directory
    json def = json({});
    config.setPath(core::args["root"].s() + "/ft8_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
    std::string resDir = core::configManager.conf["resourcesDirectory"];
    loadCTY(resDir + "/cty/cty.dat", "", cty);
    loadCTY(resDir + "/cty/AF_cty.dat", ", AF", cty);
    loadCTY(resDir + "/cty/BY_cty.dat", ", CN", cty);
    loadCTY(resDir + "/cty/EU_cty.dat", ", EU", cty);
    loadCTY(resDir + "/cty/NA_cty.dat", ", NA", cty);
    loadCTY(resDir + "/cty/SA_cty.dat", ", SA", cty);
    loadCTY(resDir + "/cty/VK_cty.dat", ", VK", cty);
    loadCTY(resDir + "/cty/cty_rus.dat", ", RUS", cty);

#ifdef __ANDROID__
    auto console_sink = std::make_shared<spdlog::sinks::android_sink_st>("SDR++");
    auto logger = std::shared_ptr<spdlog::logger>(new spdlog::logger("", { console_sink }));
    spdlog::set_default_logger(logger);
#endif

}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new FT8DecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (FT8DecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
