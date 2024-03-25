#include "interchange_context.hpp"
#include "omni_rig_com.hpp"

#include <utils/proto/rigctl.h>
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <config.h>
#include <cctype>
#include <radio_interface.h>
#include <gui/style.h>
#include <gui/smgui.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "omnirig_client",
    /* Description:     */ "Client for the Omni-Rig CAT control protocol",
    /* Author:          */ "Alexander <ra9yer@yahoo.com>",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

class OmniRIgClientModule : public ModuleManager::Instance {
public:
    OmniRIgClientModule(std::string name) {
        this->name = name;

        // Load config
        config.acquire();
        if (config.conf[name].contains("omniRigRadioIndex")) {
            omniRigRadioIndex = config.conf[name]["omniRigRadioIndex"];
            omniRigRadioIndex = std::clamp<int>(omniRigRadioIndex, 0, 1);
        }
        if (config.conf[name].contains("ifFreq")) {
            ifFreq = config.conf[name]["ifFreq"];
        }
        if (config.conf[name].contains("usePanadapterMode")) {
            usePanadapterMode = config.conf[name]["usePanadapterMode"];
        }
        if (config.conf[name].contains("selectedVfo")) {
            selectedVfo = config.conf[name]["selectedVfo"];
        }
        if (config.conf[name].contains("autoStart")) {
            autoStart = config.conf[name]["autoStart"];
        }

        config.release();

        _retuneHandler.ctx = this;
        _retuneHandler.handler = retuneHandler;

        gui::menu.registerEntry(name, menuHandler, this, NULL);
    }

    ~OmniRIgClientModule() {
        stop();
        gui::menu.removeEntry(name);
    }

    void postInit() {
        const char* radios[2] = { "Radio1", "Radio2" };
        for (const char* name : radios) {
            txtAllOmniRadios += name;
            txtAllOmniRadios += '\0';
        }

        refreshVFOs();

        {
            auto vfoBefore = selectedVfo;
            selectVfoByName(selectedVfo);
            if (vfoBefore != selectedVfo) {
                saveCurrent();
            }
        }

        // Bind handlers
        vfoCreatedHandler.handler = _vfoCreatedHandler;
        vfoCreatedHandler.ctx = this;
        vfoDeletedHandler.handler = _vfoDeletedHandler;
        vfoDeletedHandler.ctx = this;
        sigpath::vfoManager.onVfoCreated.bindHandler(&vfoCreatedHandler);
        sigpath::vfoManager.onVfoDeleted.bindHandler(&vfoDeletedHandler);

        if (autoStart) {
            start();
        }
    }

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    void start() {
        if (running) { return; }

        std::lock_guard<std::recursive_mutex> lck(mtx);

        // Switch source to panadapter mode
        if (usePanadapterMode) {
            sigpath::sourceManager.setPanadapterIF(ifFreq);
            sigpath::sourceManager.setTuningMode(SourceManager::TuningMode::PANADAPTER);
        }
        sigpath::sourceManager.onRetune.bindHandler(&_retuneHandler);

        _workerThread = std::thread(_worker, this);
    }

    void stop() {
        if (!running) { return; }

        {
            auto ctx = _interchange_ctx.lock();
            if (!ctx) {
                flog::error("can't upgrade _interchange_ctx. unexpected situation");
                return;
            }
            ctx->raise_flag_stop();
        }

        std::lock_guard<std::recursive_mutex> lck(mtx);
        if (_workerThread.joinable()) {
            flog::warn("_workerThread is joinable. before join");
            _workerThread.join();
            flog::warn("after join");
        }

        // Switch source back to normal mode
        sigpath::sourceManager.onRetune.unbindHandler(&_retuneHandler);
        if (usePanadapterMode) {
            sigpath::sourceManager.setTuningMode(SourceManager::TuningMode::NORMAL);
        }
    }

    void refreshVFOs() {
        std::lock_guard lck(vfoMtx);

        vfoNamesTxt.clear();
        vfoNamesVec.clear();

        // Add Null VFO first
        vfoNamesVec.push_back("None");
        vfoNamesTxt += vfoNamesVec[0];
        vfoNamesTxt += '\0';

        // List VFOs
        for (auto const& [_name, vfo] : gui::waterfall.vfos) {
            vfoNamesVec.push_back(_name);
            vfoNamesTxt += _name;
            vfoNamesTxt += '\0';
        }
    }

    void selectVfoByName(std::string _name) {

        // Find the ID of the VFO, if not found, select first VFO in the list
        auto vfoIt = std::find(vfoNamesVec.begin(), vfoNamesVec.end(), _name);
        if (vfoIt == vfoNamesVec.end()) {
            // fallback to None VFO
            vfoId = 0;
            selectedVfo = vfoNamesVec[0];
        }
        else {
            vfoId = std::distance(vfoNamesVec.begin(), vfoIt);
            selectedVfo = _name;
        }
    }

    static void _vfoCreatedHandler(VFOManager::VFO* vfo, void* ctx) {
        OmniRIgClientModule* _this = (OmniRIgClientModule*)ctx;
        _this->refreshVFOs();
        _this->selectVfoByName(_this->selectedVfo);
    }

    static void _vfoDeletedHandler(std::string _name, void* ctx) {
        OmniRIgClientModule* _this = (OmniRIgClientModule*)ctx;
        _this->refreshVFOs();
        _this->selectVfoByName(_this->selectedVfo);
    }


    void saveCurrent() {
        json conf;
        conf["omniRigRadioIndex"] = omniRigRadioIndex;
        conf["ifFreq"] = ifFreq;
        conf["usePanadapterMode"] = usePanadapterMode;
        conf["selectedVfo"] = selectedVfo;
        conf["autoStart"] = autoStart;
        config.acquire();
        config.conf[name] = conf;
        config.release(true);
    }


private:
    enum class SubStatus {
        None,
        Initializing,
        Error,
    };
    static void menuHandler(void* ctx) {
        OmniRIgClientModule* _this = (OmniRIgClientModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (_this->running) { style::beginDisabled(); }

        ImGui::LeftLabel("OmniRig Radio : ");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (SmGui::Combo(CONCAT("##_omni_radio_select_", _this->name), &_this->omniRigRadioIndex, _this->txtAllOmniRadios.c_str())) {
            _this->saveCurrent();
        }

        if (SmGui::Checkbox((std::string("Use Pnadapter Mode##_use_pandapater_") + _this->name).c_str(), &_this->usePanadapterMode)) {
            _this->saveCurrent();
        }

        if (_this->running) { style::endDisabled(); }

        if (!_this->usePanadapterMode) { style::beginDisabled(); }
        ImGui::LeftLabel("IF Frequency");
        ImGui::FillWidth();
        if (ImGui::InputDouble(CONCAT("##_rigctl_if_freq_", _this->name), &_this->ifFreq, 100.0, 100000.0, "%.0f")) {
            if (_this->running) {
                sigpath::sourceManager.setPanadapterIF(_this->ifFreq);
            }
            _this->saveCurrent();
        }
        if (!_this->usePanadapterMode) { style::endDisabled(); }

        ImGui::LeftLabel("Controlled VFO");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        {
            if (ImGui::Combo(CONCAT("##_rigctl_srv_vfo_", _this->name), &_this->vfoId, _this->vfoNamesTxt.c_str())) {
                std::lock_guard lck(_this->vfoMtx);
                _this->selectVfoByName(_this->vfoNamesVec[_this->vfoId]);
                _this->saveCurrent();
            }
        }

        if (ImGui::Checkbox(CONCAT("Activate on startup##_rigctl_srv_auto_lst_", _this->name), &_this->autoStart)) {
            _this->saveCurrent();
        }

        ImGui::FillWidth();
        if (_this->running && ImGui::Button(CONCAT("Stop##_rigctl_cli_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->stop();
        }
        else if (!_this->running && ImGui::Button(CONCAT("Start##_rigctl_cli_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->start();
        }

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        if (_this->running) {
            ImGui::TextColored(ImVec4(0.0, 1.0, 0.0, 1.0), "Running");
        }
        else {
            if (_this->subStatus == SubStatus::Initializing) {
                ImGui::TextColored(ImVec4(1.0, 1.0, 0.0, 1.0), "Initializing");
            }
            else if (_this->subStatus == SubStatus::Error) {
                ImGui::TextColored(ImVec4(1.0, 0.0, 0.0, 1.0), "Error");
            }
            else {
                ImGui::TextUnformatted("Idle");
            }
        }
    }

    static void _omni_rig_com_worker(OmniRIgClientModule* _this, omnirig::OmniRigCom* omni) {
        auto res = omni->init();
        if (!res.first) {
            flog::error("OmniRig init error: {0}", res.second.c_str());
            omni->setWasStopped(true);
            return;
        }

        if (omni->wasStopped()) {
            flog::warn("OmniRIg wasStopped signalled");
            return;
        }

        res = omni->working_loop();
        if (!res.first) {
            flog::error("OmniRg working_loop error: {0}", res.second.c_str());
        }

        omni->setWasStopped(true);
    }

    static void _worker(OmniRIgClientModule* _this) {

        _this->subStatus = SubStatus::Initializing;
        // we protect section until "running" flag up
        std::unique_lock<std::recursive_mutex> lck(_this->mtx);

        std::shared_ptr<omnirig::InterchangeContext> ctx(new omnirig::InterchangeContext());
        omnirig::OmniRigCom omniRigCom(ctx, _this->omniRigRadioIndex);

        if (_this->running) {
            flog::error("_this->running==true. How we appear here?");
            return;
        }
        _this->_interchange_ctx = ctx;


        auto omniThread = std::thread(_omni_rig_com_worker, _this, &omniRigCom);
        omniRigCom.wait_for_init();
        if (!omniRigCom.is_initialized()) {
            _this->subStatus = SubStatus::Error;
            flog::error("Unable to initialize _omni_rig_com_worker within reasonable time!");
            omniRigCom.setWasStopped(true);
            omniRigCom.stop();
            omniThread.join();
            _this->_workerThread.detach();
            return;
        }

        _this->running = true;
        _this->subStatus = SubStatus::None;
        lck.unlock();


        flog::info("omnirig_client: worker started");

        while (true) {
            std::unique_lock<std::mutex> lck(ctx->mtx);
            const std::chrono::duration<int64_t, std::milli> timeout{ 500LL };
            auto pred = [&ctx, &omniRigCom]() { return ctx->flag_stop || !ctx->command_list.empty(); };
            ctx->notifier.wait_for(lck, timeout, pred);

            if (ctx->flag_stop) {
                break;
            }
            if (omniRigCom.wasStopped()) {
                break;
            }

            while (!ctx->command_list.empty()) {
                const auto item = ctx->command_list.front();
                ctx->command_list.pop_front();

                if (item.cmd == omnirig::OmniCommand::ChangeFrequency_Omni_to_SDR) {
                    if (_this->vfoId) {
                        tuner::tune(tuner::TUNER_MODE_NORMAL, _this->selectedVfo, item.lParam);
                    }
                }
                else if (item.cmd == omnirig::OmniCommand::ChangeFrequency_SDR_to_Omni) {
                    omniRigCom.setFrequency(item.lParam);
                }
                else if (item.cmd == omnirig::OmniCommand::Omni_StatusChanged) {
                    flog::info("OmniRig status changed to '{0}'", item.sParam.c_str());
                }
                else if (item.cmd == omnirig::OmniCommand::Omni_ParamsChanged) {
                    flog::info("OmniRig params changed '{0}'", static_cast<int>(item.lParam));
                }
                else if (item.cmd == omnirig::OmniCommand::Omni_RxModeChanged) {
                    if (_this->vfoId) {
                        int newMode = static_cast<int>(item.rxMode);
                        core::modComManager.callInterface(_this->selectedVfo, RADIO_IFACE_CMD_SET_MODE, &newMode, NULL);
                    }
                }
                else {
                    flog::warn("unhandled command: {0} - {1},'{2}'", static_cast<int>(item.cmd), static_cast<int>(item.lParam), item.sParam.c_str());
                }
            }
        }

        omniRigCom.stop();
        omniThread.join();

        _this->running = false;
        flog::info("omnirig_client: worker stopped");
    }


    static void retuneHandler(double _freq, void* ctx) {
        OmniRIgClientModule* _this = (OmniRIgClientModule*)ctx;
        auto interchnage_ctx = _this->_interchange_ctx.lock();
        if (!interchnage_ctx) {
            return;
        }

        // Get center frequency of the SDR
        double freq = gui::waterfall.getCenterFrequency();

        // Add the offset of the VFO if it exists
        if (sigpath::vfoManager.vfoExists(_this->selectedVfo)) {
            freq += sigpath::vfoManager.getOffset(_this->selectedVfo);
        }
        flog::info("--- retune to {0} {1}", _freq, freq);

        omnirig::CmdItem item;
        item.cmd = omnirig::OmniCommand::ChangeFrequency_SDR_to_Omni;
        item.lParam = static_cast<long>(freq);
        interchnage_ctx->put_command(std::move(item));
    }

    std::string name;
    bool enabled = true;
    bool running = false;
    SubStatus subStatus = SubStatus::None;
    std::recursive_mutex mtx;
    //
    std::weak_ptr<omnirig::InterchangeContext> _interchange_ctx;
    std::thread _workerThread;

    //
    EventHandler<VFOManager::VFO*> vfoCreatedHandler;
    EventHandler<std::string> vfoDeletedHandler;

    //
    std::string txtAllOmniRadios;
    int omniRigRadioIndex = 0; // Radio1 by default
    bool usePanadapterMode = false;
    bool autoStart = false;
    //
    std::mutex vfoMtx;
    std::vector<std::string> vfoNamesVec;
    int vfoId = 0;
    std::string selectedVfo;
    std::string vfoNamesTxt;


    double ifFreq = 8830000.0;

    EventHandler<double> _retuneHandler;
};

MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/omnirig_client_config.json");
    json defConf;
    config.load(defConf);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new OmniRIgClientModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (OmniRIgClientModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
