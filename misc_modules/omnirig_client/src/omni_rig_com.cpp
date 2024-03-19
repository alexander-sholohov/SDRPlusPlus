#include "omni_rig_com.hpp"

#include <Windows.h>
#include <atlbase.h>
#include <atlcom.h>

// clang-format off
#define ENSURE_HR_OK_RETURN_RES(x, reason) do { if (x != S_OK) return {false, reason}; } while(0)
#define ENSURE_HR_OK_SET_CTX(x, ec, reason) do { if (x != S_OK) {ec.first = false; ec.second=reason; return;} } while(0)
// clang-format on

using namespace omnirig;

constexpr UINT WM_MYMESSAGE_SDR_TO_OMNI = WM_APP + 10;
constexpr UINT WM_MYMESSAGE_OMNI_TO_SDR_VISIBLE_CHANGE = WM_APP + 11;
constexpr UINT WM_MYMESSAGE_OMNI_TO_SDR_RIG_TYPE_CHANGE = WM_APP + 12;
constexpr UINT WM_MYMESSAGE_OMNI_TO_SDR_STATUS_CHANGE = WM_APP + 13;
constexpr UINT WM_MYMESSAGE_OMNI_TO_SDR_PARAMS_CHANGE = WM_APP + 14;


constexpr IID IID_OmniEvents{ 0x2219175F, 0xE561, 0x47E7, { 0xad, 0x17, 0x73, 0xc4, 0xd8, 0x89, 0x1a, 0xa1 } };

static _ATL_FUNC_INFO ArgsNoArgs = { CC_STDCALL, VT_EMPTY, 0 };
static _ATL_FUNC_INFO ArgsRigNumberOnly = { CC_STDCALL, VT_EMPTY, 1, { VT_I4 } };
static _ATL_FUNC_INFO ArgsRigNumberParams = { CC_STDCALL, VT_EMPTY, 2, { VT_I4, VT_I4 } };
static _ATL_FUNC_INFO ArgsRigNumberVariants = { CC_STDCALL, VT_EMPTY, 2, { VT_VARIANT, VT_VARIANT } };


// clang-format off
class CMySink : public IDispEventSimpleImpl<1, CMySink, &IID_OmniEvents> {
public:
    CMySink(std::weak_ptr<InterchangeContext> ctx, DWORD threadId) : m_ctx(ctx), m_threadId(threadId) {}

    BEGIN_SINK_MAP(CMySink)
    SINK_ENTRY_INFO(1, IID_OmniEvents, 1, OnVisibleChange, &ArgsNoArgs)
    SINK_ENTRY_INFO(1, IID_OmniEvents, 2, OnRigTypeChange, &ArgsRigNumberOnly)
    SINK_ENTRY_INFO(1, IID_OmniEvents, 3, OnStatusChange, &ArgsRigNumberOnly)
    SINK_ENTRY_INFO(1, IID_OmniEvents, 4, OnParamsChange, &ArgsRigNumberParams)
    SINK_ENTRY_INFO(1, IID_OmniEvents, 5, OnCustomReply, &ArgsRigNumberVariants)
    END_SINK_MAP()

    STDMETHOD(OnVisibleChange)() {
        ::PostThreadMessage(m_threadId, WM_MYMESSAGE_OMNI_TO_SDR_VISIBLE_CHANGE, 0, 0);
        return S_OK;
    }

    STDMETHOD(OnRigTypeChange)(long rigNumber) {
        ::PostThreadMessage(m_threadId, WM_MYMESSAGE_OMNI_TO_SDR_RIG_TYPE_CHANGE, (WPARAM)rigNumber, 0);
        return S_OK;
    }

    STDMETHOD(OnStatusChange)(long rigNumber) {
        ::PostThreadMessage(m_threadId, WM_MYMESSAGE_OMNI_TO_SDR_STATUS_CHANGE, (WPARAM)rigNumber, 0);
        return S_OK;
    }

    STDMETHOD(OnParamsChange)(long rigNumber, long params) {
        ::PostThreadMessage(m_threadId, WM_MYMESSAGE_OMNI_TO_SDR_PARAMS_CHANGE, (WPARAM)rigNumber, (LPARAM)params);
        return S_OK;
    }

    STDMETHOD(OnCustomReply)(long rigNumber, VARIANT command, VARIANT reply) {
        return S_OK;
    }

private:
    std::weak_ptr<InterchangeContext> m_ctx;
    DWORD m_threadId;
};
// clang-format on


static std::string bstr2s(const CComBSTR& s) {
    std::wstring ws(s, s.Length());
    std::string str;
    for (auto& ch : ws) {
        str += (char)ch;
    }

    return str;
}


OmniRigCom::OmniRigCom(std::shared_ptr<InterchangeContext>& interchange_context, int rig_number)
    : m_ctx(interchange_context), m_rig_number(rig_number){};


MyResult OmniRigCom::init() {
    HRESULT hr;
    hr = ::CoInitialize(NULL);
    ENSURE_HR_OK_RETURN_RES(hr, "CoInitialize error");

    hr = m_omniEngine.CoCreateInstance(__uuidof(OmniRigX));
    ENSURE_HR_OK_RETURN_RES(hr, "OmniRig.OmniRigX create error");

    if (m_rig_number == 0) {
        hr = m_omniEngine->get_Rig1(&m_rig);
    }
    else {
        hr = m_omniEngine->get_Rig2(&m_rig);
    }
    ENSURE_HR_OK_RETURN_RES(hr, "OmniRig get_Rig error");

    m_currentThreadId = GetCurrentThreadId();

    return { true, "" };
}

void OmniRigCom::processSDRToOmniMessage(WPARAM wParam, LPARAM lParam, MyResult& err_context) {
    HRESULT hr;

    if (wParam == (WPARAM)OmniCommand::ChangeFrequency_SDR_to_Omni) {
        long freq = static_cast<long>(lParam);
        hr = m_rig->put_Freq(freq);
        ENSURE_HR_OK_SET_CTX(hr, err_context, "PutProperty frequency error");
    }
}

void OmniRigCom::processStatusChanged(MyResult& err_context) {
    CComBSTR status;
    HRESULT hr = m_rig->get_StatusStr(&status);
    ENSURE_HR_OK_SET_CTX(hr, err_context, "OmniRig get_StatusStr error");

    auto ctx = m_ctx.lock();
    if (!ctx) {
        return;
    }

    CmdItem item;
    item.cmd = OmniCommand::Omni_StatusChanged;
    item.sParam = bstr2s(status);
    ctx->put_command(std::move(item));
}


void OmniRigCom::processParamsChanged(long params, MyResult& err_context) {
    HRESULT hr;

    auto ctx = m_ctx.lock();
    if (!ctx) {
        return;
    }

    bool is_param_known = false;

    // we treat params as bit mapped data.

    if (params & PM_FREQ) {
        long freq = 0;
        hr = m_rig->get_Freq(&freq);
        ENSURE_HR_OK_SET_CTX(hr, err_context, "OmniRing get_Freq error");

        CmdItem item;
        item.cmd = OmniCommand::ChangeFrequency_Omni_to_SDR;
        item.lParam = freq;
        ctx->put_command(std::move(item));

        is_param_known = true;
    }

    if (params & PM_SSB_U) {
        CmdItem item;
        item.cmd = OmniCommand::Omni_RxModeChanged;
        item.rxMode = RxMode::SSB_U;
        ctx->put_command(std::move(item));

        is_param_known = true;
    }


    if (params & PM_SSB_L) {
        CmdItem item;
        item.cmd = OmniCommand::Omni_RxModeChanged;
        item.rxMode = RxMode::SSB_L;
        ctx->put_command(std::move(item));

        is_param_known = true;
    }

    if (params & PM_AM) {
        CmdItem item;
        item.cmd = OmniCommand::Omni_RxModeChanged;
        item.rxMode = RxMode::AM;
        ctx->put_command(std::move(item));
        is_param_known = true;
    }

    if (params & PM_FM) {
        CmdItem item;
        item.cmd = OmniCommand::Omni_RxModeChanged;
        item.rxMode = RxMode::NFM;
        ctx->put_command(std::move(item));
        is_param_known = true;
    }

    if (params & (PM_CW_L | PM_CW_U)) {
        CmdItem item;
        item.cmd = OmniCommand::Omni_RxModeChanged;
        item.rxMode = RxMode::CW;
        ctx->put_command(std::move(item));
        is_param_known = true;
    }

    if (!is_param_known) {
        CmdItem item;
        item.cmd = OmniCommand::Omni_ParamsChanged;
        item.lParam = params;
        ctx->put_command(std::move(item));
    }
}

MyResult OmniRigCom::working_loop() {
    HRESULT hr;
    CMySink theSink(m_ctx, m_currentThreadId);
    hr = theSink.DispEventAdvise(m_omniEngine);
    ENSURE_HR_OK_RETURN_RES(hr, "DispEventAdvise error");

    MyResult res{ true, "" };

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_MYMESSAGE_SDR_TO_OMNI) {
            processSDRToOmniMessage(msg.wParam, msg.lParam, res);
        }
        else if (msg.message == WM_MYMESSAGE_OMNI_TO_SDR_VISIBLE_CHANGE) {
            // nothing
        }
        else if (msg.message == WM_MYMESSAGE_OMNI_TO_SDR_RIG_TYPE_CHANGE) {
            // nothing
        }
        else if (msg.message == WM_MYMESSAGE_OMNI_TO_SDR_STATUS_CHANGE) {
            processStatusChanged(res);
        }
        else if (msg.message == WM_MYMESSAGE_OMNI_TO_SDR_PARAMS_CHANGE) {
            processParamsChanged(static_cast<long>(msg.lParam), res);
        }
        else {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // break on first error
        if (!res.first) {
            break;
        }
    }

    hr = theSink.DispEventUnadvise(m_omniEngine);
    ENSURE_HR_OK_RETURN_RES(hr, "DispEventUnadvise error");

    return res;
}

void OmniRigCom::setWasStopped(bool value) {
    m_was_stopped = value;
}

bool OmniRigCom::wasStopped() const {
    return m_was_stopped;
}

void OmniRigCom::stop() {
    ::PostThreadMessage(m_currentThreadId, WM_QUIT, 0, 0);
}

void OmniRigCom::setFrequency(long freq) {
    ::PostThreadMessage(m_currentThreadId, WM_MYMESSAGE_SDR_TO_OMNI, (WPARAM)OmniCommand::ChangeFrequency_SDR_to_Omni, (LPARAM)freq);
}
