#include "interchange_context.hpp"

using namespace omnirig;

void InterchangeContext::raise_flag_stop() {
    flag_stop = true;
    notifier.notify_all();
}

void InterchangeContext::put_command(CmdItem cmd) {
    {
        std::scoped_lock<std::mutex> lck(mtx);
        command_list.push_back(std::move(cmd));
    }
    notifier.notify_all();
}
