#pragma once

#include <mutex>
#include <deque>

namespace omnirig {

    enum class OmniCommand {
        Stop = 0,
        ChangeFrequency_SDR_to_Omni,
        ChangeFrequency_Omni_to_SDR,
        Omni_StatusChanged,
        Omni_ParamsChanged,
        Omni_RxModeChanged,
    };


    // value must be equal to enum DemodID in radio_module.h
    enum class RxMode {
        NFM = 0, // narrow FM
        AM = 2,
        SSB_U = 4,
        CW = 5,
        SSB_L = 6,
    };

    struct CmdItem {
        OmniCommand cmd;
        float fParam{};
        long lParam{};
        std::string sParam;
        RxMode rxMode;
    };

    struct InterchangeContext {
        bool flag_stop{};
        std::mutex mtx;
        std::deque<CmdItem> command_list;
        std::condition_variable notifier;

        void raise_flag_stop();
        void put_command(CmdItem cmd);
    };

}