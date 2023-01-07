
#define _WINSOCKAPI_    // stops windows.h including winsock.h

#include <imgui.h>
#include <gui/smgui.h>
#include <spdlog/spdlog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <config.h>
#include <iostream>
#include <spdlog/sinks/android_sink.h>

#include "hl2_device.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO {
    /* Name:            */ "hl2_source",
    /* Description:     */ "Hermes Lite 2 module for SDR++",
    /* Author:          */ "sannysanoff",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

//const char* AGG_MODES_STR = "Off\0Low\0High\0";

std::string discoveredToIp(DISCOVERED &d) {
    char str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(d.info.network.address.sin_addr), str, INET_ADDRSTRLEN);
    return str;
}

class HermesLite2SourceModule : public ModuleManager::Instance, public Transmitter {

    int adcGain = 0;
    bool sevenRelays[10];

public:

    HermesLite2SourceModule(std::string name) {

#ifdef __ANDROID__
        auto console_sink = std::make_shared<spdlog::sinks::android_sink_st>("SDR++/hl2_source");
        auto logger = std::shared_ptr<spdlog::logger>(new spdlog::logger("", { console_sink }));
        spdlog::set_default_logger(logger);
#endif


        this->name = name;
        memset(sevenRelays, 0, sizeof(sevenRelays));

        sampleRate = 48000;

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;

        refresh();

        config.acquire();
        std::string devSerial = config.conf["device"];
        config.release();

        sigpath::sourceManager.registerSource("Hermes Lite 2", &handler);
        selectFirst();


    }

    ~HermesLite2SourceModule() {
        stop(this);
        sigpath::sourceManager.unregisterSource("Hermes Lite 2");
    }

    void postInit() {}


    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    void refresh() {

        devices = 0;
        protocol1_discovery();

        devListTxt = "";

        for (int i = 0; i < devices; i++) {
            auto ip = discoveredToIp(discovered[i]);

            devListTxt += ip +" - " ;
            if (discovered[i].device == DEVICE_HERMES_LITE2 || discovered[i].device == DEVICE_HERMES_LITE) {
                devListTxt += std::to_string(discovered[i].supported_receivers)+" * ";
            }
            devListTxt+=discovered[i].name;

            devListTxt += '\0';
        }
    }

    void selectFirst() {
        if (devices != 0) {
            selectByIP(discoveredToIp(discovered[0]));
        }
    }

    void selectByIP(std::string ipAddr) {

        selectedIP = ipAddr;

        sampleRateList.clear();
        sampleRateListTxt = "";
        sampleRateList.push_back(48000);
        sampleRateList.push_back(96000);
        sampleRateList.push_back(192000);
        sampleRateList.push_back(384000);
        for(auto sr: sampleRateList) {
            sampleRateListTxt += std::to_string(sr);
            sampleRateListTxt += '\0';
        }

        selectedSerStr = ipAddr;

        // Load config here
        config.acquire();
        bool created = false;
        if (!config.conf["devices"].contains(selectedSerStr)) {
            created = true;
            config.conf["devices"][selectedSerStr]["sampleRate"] = sampleRateList[0];
        }

        // Load sample rate
        srId = 0;
//        sampleRate = sampleRateList[3];
        if (config.conf["devices"][selectedSerStr].contains("sampleRate")) {
            int selectedSr = config.conf["devices"][selectedSerStr]["sampleRate"];
            for (int i = 0; i < sampleRateList.size(); i++) {
                if (sampleRateList[i] == selectedSr) {
                    srId = i;
                    sampleRate = selectedSr;
                    break;
                }
            }
        }

        // Load Gains
//        if (config.conf["devices"][selectedSerStr].contains("agcMode")) {
//            agcMode = config.conf["devices"][selectedSerStr]["agcMode"];
//        }
//        if (config.conf["devices"][selectedSerStr].contains("lna")) {
//            hfLNA = config.conf["devices"][selectedSerStr]["lna"];
//        }
//        if (config.conf["devices"][selectedSerStr].contains("attenuation")) {
//            atten = config.conf["devices"][selectedSerStr]["attenuation"];
//        }

        config.release(created);

//        airspyhf_close(dev);
    }

private:

    static void menuSelected(void* ctx) {
        HermesLite2SourceModule* _this = (HermesLite2SourceModule*)ctx;
        core::setInputSampleRate(_this->sampleRate);
        spdlog::info("HermerList2SourceModule '{0}': Menu Select!", _this->name);
    }

    static void txmenuSelected(void* ctx) {
        HermesLite2SourceModule* _this = (HermesLite2SourceModule*)ctx;
//        core::setInputSampleRate(_this->sampleRate);
        spdlog::info("HermerList2SourceModule '{0}': TX Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        HermesLite2SourceModule* _this = (HermesLite2SourceModule*)ctx;
        spdlog::info("HermerList2SourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void txmenuDeselected(void* ctx) {
        HermesLite2SourceModule* _this = (HermesLite2SourceModule*)ctx;
        spdlog::info("HermerList2SourceModule '{0}': TX Menu Deselect!", _this->name);
    }

    std::vector<dsp::complex_t> incomingBuffer;

    void incomingSample(double i, double q) {
        incomingBuffer.emplace_back(dsp::complex_t{(float)q, (float)i});
        if (incomingBuffer.size() >= 2048) {
            memcpy(stream.writeBuf, incomingBuffer.data(), incomingBuffer.size() * sizeof(dsp::complex_t));
            stream.swap(incomingBuffer.size());
            incomingBuffer.clear();
        }

    }

    static void start(void* ctx) {
        HermesLite2SourceModule* _this = (HermesLite2SourceModule*)ctx;
        if (_this->running) {
            return; }
        if (_this->selectedIP.empty()) {
            spdlog::error("Tried to start HL2 source with null serial");
            return;
        }

        _this->device.reset();
        for(int i=0; i<devices; i++) {
            if (_this->selectedIP == discoveredToIp(discovered[i])) {
                _this->device = std::make_shared<HL2Device>(discovered[i], [=](double i, double q) {
                    static auto lastCtm = currentTimeMillis();
                    static auto totalCount = 0LL;
                    totalCount++;
                    if (totalCount % 10000 == 0) {
                        if (lastCtm < currentTimeMillis() - 1000) {
                            static auto lastLastTotalCount = 0LL;
                            auto nowCtm = currentTimeMillis();
//                            std::cout << "HL2: Speed: IQ pairs/sec: ~ " << (totalCount - lastLastTotalCount) * 1000 / (nowCtm - lastCtm) << std::endl;
                            lastLastTotalCount = totalCount;
                            lastCtm = nowCtm;
                        }
                    }
                    _this->incomingSample(i, q);
                });
            }

        }

        if (_this->device) {
            _this->device->setRxSampleRate(_this->sampleRate);
            _this->device->setADCGain(_this->adcGain);
            _this->device->start();
        }
        _this->running = true;
        sigpath::transmitter = _this;
        spdlog::info("HL2SourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        HermesLite2SourceModule* _this = (HermesLite2SourceModule*)ctx;
        if (!_this->running) { return; }
        _this->running = false;
        _this->device->stop();
        _this->stream.stopWriter();
        _this->stream.clearWriteStop();
        spdlog::info("HermerList2SourceModule '{0}': Stop!", _this->name);
        _this->device.reset();

        sigpath::transmitter = nullptr;
    }

    static void tune(double freq, void* ctx) {
        HermesLite2SourceModule* _this = (HermesLite2SourceModule*)ctx;
        _this->freq = freq;
        if (_this->device) {
            _this->device->setFrequency((int)freq);
        }
        spdlog::info("HermerList2SourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void txtune(double freq, void* ctx) {
        HermesLite2SourceModule* _this = (HermesLite2SourceModule*)ctx;
        _this->txfreq = freq;
        if (_this->device) {
            _this->device->setTxFrequency((int)freq);
        }
        spdlog::info("HermerList2SourceModule '{0}': TxTune: {1}!", _this->name, freq);
    }

    bool hardTune = false;

    static void txmenuHandler(void* ctx) {
        HermesLite2SourceModule* _this = (HermesLite2SourceModule*)ctx;
        if (!_this->running) { SmGui::BeginDisabled(); }

        int drawHardTune = _this->hardTune;
        if (drawHardTune) {
            SmGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0, 0, 1.0f));
            SmGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0, 0, 1.0f));
            SmGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0, 0, 1.0f));
        }
        if (SmGui::Button("Hard Tune")) {
            _this->hardTune = !_this->hardTune;
            _this->device->setTxFrequency((int) _this->freq);
            _this->device->doTuneActive(_this->hardTune);
            std::cout << "_this->hardTune=" << _this->hardTune << std::endl;
        }
        if (drawHardTune) {
            SmGui::PopStyleColor(3);
        }

        if (!_this->running) { SmGui::EndDisabled(); }

    }

    static void menuHandler(void* ctx) {
        HermesLite2SourceModule* _this = (HermesLite2SourceModule*)ctx;

        if (_this->running) { SmGui::BeginDisabled(); }

//        SmGui::SetNextItemWidth(100);
        if (SmGui::Combo(CONCAT("##_hl2_dev_sel_", _this->name), &_this->devId, _this->devListTxt.c_str())) {
            _this->selectByIP(discoveredToIp(discovered[_this->devId]));
            core::setInputSampleRate(_this->sampleRate);
            if (_this->selectedSerStr != "") {
                config.acquire();
                config.conf["device"] = _this->selectedSerStr;
                config.release(true);
            }
        }

        auto updateSampleRate = [&](int srid) {
            _this->sampleRate = _this->sampleRateList[_this->srId];
            core::setInputSampleRate(_this->sampleRate);
            if (_this->selectedSerStr != "") {
                config.acquire();
                config.conf["devices"][_this->selectedSerStr]["sampleRate"] = _this->sampleRate;
                config.release(true);
            }

        };
        if (SmGui::Combo(CONCAT("##_hl2_sr_sel_", _this->name), &_this->srId, _this->sampleRateListTxt.c_str())) {
            updateSampleRate(_this->srId);
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_hl2_refr_", _this->name))) {
            _this->refresh();
//            config.acquire();
//            std::string devSerial = config.conf["device"];
//            config.release();
            if (devices > 0) {
                _this->selectFirst();
            }
        }

        if (_this->running) { SmGui::EndDisabled(); }
        bool overload = _this->device && _this->device->isADCOverload();
        if (overload) {
            SmGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0, 0, 1.0f));
        }
        SmGui::LeftLabel("ADC Gain");
        if (overload) {
            SmGui::PopStyleColor(1);
        }
        SmGui::SameLine();
//        SmGui::SetNextItemWidth(100);
        if (SmGui::SliderInt(("##_radio_agc_gain_" + _this->name).c_str(), &_this->adcGain, -12, +48, SmGui::FMT_STR_INT_DB)) {
            if (_this->device) {
                _this->device->setADCGain(_this->adcGain);
            }
        }
        for(int q=0; q<7; q++) {
            char strr[100];
            sprintf(strr, "%d", q);
            if (SmGui::RadioButton(strr, _this->sevenRelays[q])) {
                if (_this->sevenRelays[q]) {
                    memset(_this->sevenRelays, 0, sizeof(_this->sevenRelays));
                    if (_this->device) {
                        _this->device->setSevenRelays(0);
                    }
                } else {
                    memset(_this->sevenRelays, 0, sizeof(_this->sevenRelays));
                    _this->sevenRelays[q] = !_this->sevenRelays[q];
                    if (_this->device) {
                        _this->device->setSevenRelays(1 << q);
                    }
                }
            }
            if (q != 6) {
                SmGui::SameLine();
            }
        }

    }

    std::string name;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    dsp::stream<dsp::complex_t> txstream;
    int sampleRate;
    SourceManager::SourceHandler handler;
    bool running = false;
    double freq;
    double txfreq;
    std::string selectedIP;
    int devId = 0;
    int srId = 0;
    float atten = 0.0f;
    std::string selectedSerStr = "";

    std::string devListTxt;
    std::vector<uint32_t> sampleRateList;
    std::string sampleRateListTxt;
    std::shared_ptr<HL2Device> device;


    int getInputStreamFramerate() override {
        return 48000;
    }
    void setTransmitStatus(bool status) override {
        device->setTune(false);
        device->setPTT(status);

    }
    void setTransmitStream(dsp::stream<dsp::complex_t>* stream) override {
        std::thread([this, stream]() {
            std::vector<dsp::complex_t> buffer;
            int addedBlocks = 0;
            int readSamples = 0;
            int nreads = 0;
            while (true) {
                int rd = stream->read();
                if (rd < 0) {
                    printf("End iq stream for tx");
                    break;
                }
                readSamples += rd;
                nreads++;
                for(int q=0; q<rd; q++) {
                    buffer.push_back(stream->readBuf[q]);
                    if (buffer.size() == 63) {
                        addedBlocks++;
                        if (addedBlocks % 1000 == 0) {
                            spdlog::info("Added {} blocks to tx buffer, rd samples {}  ndreads {}", addedBlocks, readSamples, nreads);
                        }
                        device->samplesToSendLock.lock();
                        device->samplesToSend.emplace_back(std::make_shared<std::vector<dsp::complex_t>>(buffer));
                        device->samplesToSendLock.unlock();
                        buffer.clear();
                    }
                }
                stream->flush();
            }
        }).detach();
    }
    void setTransmitGain(unsigned char gain) override {
        device->setPower(gain);

    }
    void setTransmitFrequency(int freq) override {
        device->setTxFrequency(freq);

    }
    void startGenerateTone(int frequency) override {
        device->setFrequency(frequency);
        device->setTxFrequency(frequency);
        device->setTune(true);
        device->setPTT(true);
    }

    void setPAEnabled(bool enabled) {
        device->setPAEnabled(enabled);
    }

    void stopGenerateTone() override {
        device->setPTT(false);
        device->setTune(false);
    }

    void setToneGain() override {
    }

    int getTXStatus() override {
        return device->transmitMode;
    }
    float getTransmitPower() override {
        return device->fwd;
//        device->getSWR();
//        return device->fwd+device->rev;
    }

public:
    float getReflectedPower() override {
        return device->rev;
    }

private:
    float getTransmitSWR() override {
        return device->swr;
    }

    float getFillLevel() override {
        return device->fill_level;
    }

    std::string& getTransmitterName() override {
        static std::string name = "Hermes Lite 2";
        return name;
    }
};

MOD_EXPORT void _INIT_() {
#ifdef WIN32
    int iResult;

// Initialize Winsock
    WSADATA wsaData;

    iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (iResult != 0) {
        spdlog::error("WSAStartup failed: %d\n", iResult);
        exit(1);
    }
#endif
    json def = json({});
    def["devices"] = json({});
    def["device"] = "";
    config.setPath(core::args["root"].s() + "/hl2_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new HermesLite2SourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (HermesLite2SourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}