#include <server.h>
#include "imgui.h"
#include <stdio.h>
#include <gui/main_window.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <gui/icons.h>
#include <version.h>
#include <utils/flog.h>
#include <gui/widgets/bandplan.h>
#include <stb_image.h>
#include <config.h>
#include <core.h>
#include <filesystem>
#include <gui/menus/theme.h>
#include <backend.h>
#include <iostream>
#include <gui/menus/display.h>

#ifdef __APPLE__
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#endif

#ifdef __linux__
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

void setproctitle(const char* fmt, ...) {
    static char title[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(title, sizeof(title), fmt, args);
    va_end(args);
    //    prctl(PR_SET_NAME, title, 0, 0, 0);
    static char dash[100];
    strcpy(dash, "--");
    char* new_argv[] = { core::args.systemArgv[0], dash, title, NULL };
    memcpy(core::args.systemArgv, new_argv, sizeof(new_argv));
}

const char *sdrppResourcesDirectory; // to reference from C files.

#else
void setproctitle(const char* fmt, ...) {
}
#endif


#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>

#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>


#endif

#ifndef INSTALL_PREFIX
#ifdef __APPLE__
#define INSTALL_PREFIX "/usr/local"
#else
#define INSTALL_PREFIX "/usr"
#endif
#endif

namespace core {
    ConfigManager configManager;
    ModuleManager moduleManager;
    ModuleComManager modComManager;
    CommandArgsParser args;

    void setInputSampleRate(double samplerate) {
        // Forward this to the server
        if (args["server"].b()) {
            server::setInputSampleRate(samplerate);
            return;
        }

        // Update IQ frontend input samplerate and get effective samplerate
        sigpath::iqFrontEnd.setSampleRate(samplerate);
        double effectiveSr = sigpath::iqFrontEnd.getEffectiveSamplerate();

        // Reset zoom
        gui::waterfall.setBandwidth(effectiveSr);
        gui::waterfall.setViewOffset(0);
        gui::mainWindow.updateZoom();

        // Debug logs
        flog::info("New DSP samplerate: {0} (source samplerate is {1})", effectiveSr, samplerate);
    }


    /// FORK SERVER

    int forkPipe[2];
    int forkResult[2];

    struct ForkServerResults {
        int seq = 0;
        int pid = 0;
        bool terminated = false;
        int wstatus = 0;
    };

    std::unordered_map<int, std::shared_ptr<SpawnCommand>> forkInProgress;
    std::mutex forkInProgressLock;

    bool forkIt(const std::shared_ptr<SpawnCommand> &cmd) {
#ifndef _WIN32
        static std::atomic_int _seq;
        forkInProgressLock.lock();
        cmd->seq = _seq++;
        cmd->completed = false;
        forkInProgress[cmd->seq] = cmd;
        forkInProgressLock.unlock();
        if (sizeof(*cmd.get()) != write(forkPipe[1], cmd.get(), sizeof(*cmd.get()))) {
            return false;
        }
#endif
        return true;
    }

    void removeForkInProgress(int seq) {
        forkInProgressLock.lock();
        forkInProgress.erase(seq);
        forkInProgressLock.unlock();
    }

    void cldHandler(int i) {
#ifndef _WIN32
//        write(1, "cldHandler\n", strlen("cldHandler\n"));
//        flog::info("SIGCLD, waiting i={}", i);
        int wstatus;
        auto q = wait(&wstatus);
//        flog::info("SIGCLD, waited = {}, status={}", q, wstatus);
        ForkServerResults res;
        res.seq = -1;
        res.pid = q;
        res.terminated = true;
        res.wstatus = wstatus;
//        flog::info("FORKSERVER, sending pid death: {}", q);
        write(forkResult[1], &res, sizeof(res));
#endif
    }

    void startForkServer() {
#ifndef _WIN32
        if (pipe(forkPipe)) {
            flog::error("Cannot create pipe.");
            exit(1);
        }
        if (pipe(forkResult)) {
            flog::error("Cannot create pipe.");
            exit(1);
        }
        if (fork() == 0) {

            setproctitle("sdrpp (sdr++) fork server (spawning decoders)");

#ifdef __linux__
//            int priority = 19; // background, the least priority
//            int which = PRIO_PROCESS; // set priority for the current process
//            pid_t pid = 0; // use the current process ID
//            int ret = setpriority(which, pid, priority);
//            if (ret != 0) {
//                // error handling
//            }
#endif

//            flog::info("FORKSERVER: fork server runs");
            int myPid = getpid();
            std::thread checkParentAlive([=]() {
                while (true) {
                    sleep(1);
                    if (getppid() == 1) {
                        kill(myPid, SIGHUP);
                        break;
                    }
                }
            });
            checkParentAlive.detach();
#ifndef SIGCLD
#define SIGCLD 17
#endif
            signal(SIGCLD, cldHandler);
            bool running = true;
            while (running) {
                SpawnCommand cmd;
                if (sizeof(cmd) != read(forkPipe[0], &cmd, sizeof(cmd))) {
                    flog::warn("FORKSERVER, misread command");
                    continue;
                }

                auto newPid = fork();
                if (0 == newPid) {
//                    flog::info("FORKSERVER {}, forked ok", cmd.info);
                    auto& args = cmd;
                    std::string execDir = args.executable;
                    auto pos = execDir.rfind('/');
                    if (pos != std::string::npos) {
                        execDir = execDir.substr(0, pos);
                        execDir = "LD_LIBRARY_PATH=" + execDir;
                        putenv((char*)execDir.c_str());
//                        flog::info("FORKSERVER, in child, before exec putenv {}", execDir);
                    }
//                    flog::info("decoderPath={}", args.executable);
//                    flog::info("FT8 Decoder({}): executing: {}", cmd.info, args.executable);

                    if (true) {
                        close(0);
                        close(1);
                        close(2);
                        open("/dev/null", O_RDONLY, 0600);                     // input
                        open(cmd.outPath, O_CREAT | O_TRUNC | O_WRONLY, 0600); // out
                        open(cmd.errPath, O_CREAT | O_TRUNC | O_WRONLY, 0600); // err
                    }
                    std::vector<char*> argsv;
                    for (int i = 0; i < args.nargs; i++) {
                        argsv.emplace_back(&args.args[i][0]);
                    }
                    argsv.emplace_back(nullptr);
                    auto err = execv((const char*)(&args.executable[0]), argsv.data());
                    static auto q = errno;
                    if (err < 0) {
                        perror("exec");
                    }
                    write(1, "\nBefore process exit\n", strlen("\nBefore process exit\n"));
                    close(0);
                    close(1);
                    close(2);
                    abort(); // exit does not terminate well.


                    flog::warn("FORKSERVER, back from forked ok");
                }
                else {
                    ForkServerResults res;
                    res.seq = cmd.seq;
                    res.pid = newPid;
//                    flog::info("FORKSERVER ({}), sending pid: {}", cmd.info, newPid);
                    write(forkResult[1], &res, sizeof(res));
                }
            }
        }
        else {
            std::thread resultReader([]() {
                SetThreadName("forkserver_resultread");
//                flog::info("FORKSERVER: resultreader started");
                while (true) {
                    ForkServerResults res;
                    if (0 != read(forkResult[0], &res, sizeof(res))) {
                        forkInProgressLock.lock();
                        auto found = forkInProgress.find(res.seq);
                        if (res.seq < 0) {
                            for (auto it : forkInProgress) {
                                if (it.second->pid == res.pid) {
                                    found = forkInProgress.find(it.first);
                                    break;
                                }
                            }
                        }
                        if (found != forkInProgress.end()) {
                            if (res.pid != 0) {
                                found->second->pid = res.pid;
                            }
                            if (res.terminated != 0) {
//                                flog::info("FORKSERVER: marking terminated: pid={}, res={}", res.pid, (void*)&res);
                                found->second->completeStatus = res.wstatus;
                                found->second->completed = true;
                            }
                        }
                        else {
//                            flog::info("FORKSERVER: not found mark status: pid={} seq={}", res.pid, res.seq);
                        }
                        forkInProgressLock.unlock();
                    }
                }
            });
            resultReader.detach();
        }
#endif
    }

};

extern void test1();

// main
int sdrpp_main(int argc, char* argv[]) {

//    test1();
#ifdef _WIN32
    setlocale(LC_ALL, ".65001"); // Set locale to UTF-8
#endif
    flog::info("SDR++Brown v" VERSION_STR);

#ifdef IS_MACOS_BUNDLE
    // If this is a MacOS .app, CD to the correct directory
    auto execPath = std::filesystem::absolute(argv[0]);
    chdir(execPath.parent_path().string().c_str());
#endif

    // Define command line options and parse arguments
    core::args.defineAll();
    if (core::args.parse(argc, argv) < 0) { return -1; }

    // Show help and exit if requested
    if (core::args["help"].b()) {
        core::args.showHelp();
        return 0;
    }

    bool serverMode = (bool)core::args["server"];

    if (!serverMode) {
        core::startForkServer();
    }

#ifdef _WIN32
    // Free console if the user hasn't asked for a console and not in server mode
#ifdef NDEBUG
    if (!core::args["con"].b() && !serverMode) { FreeConsole(); }
#endif

    // Set error mode to avoid abnoxious popups
    SetErrorMode(SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
#endif

    // Check root directory
    std::string root = (std::string)core::args["root"];
    if (!std::filesystem::exists(root)) {
        flog::warn("Root directory {0} does not exist, creating it", root);
        if (!std::filesystem::create_directories(root)) {
            flog::error("Could not create root directory {0}", root);
            return -1;
        }
    }

    // Check that the path actually is a directory
    if (!std::filesystem::is_directory(root)) {
        flog::error("{0} is not a directory", root);
        return -1;
    }

    // ======== DEFAULT CONFIG ========
    json defConfig;
    defConfig["bandColors"]["amateur"] = "#FF0000FF";
    defConfig["bandColors"]["aviation"] = "#00FF00FF";
    defConfig["bandColors"]["broadcast"] = "#0000FFFF";
    defConfig["bandColors"]["marine"] = "#00FFFFFF";
    defConfig["bandColors"]["military"] = "#FFFF00FF";
    defConfig["bandPlan"] = "General";
    defConfig["bandPlanEnabled"] = true;
    defConfig["bandPlanPos"] = 0;
    defConfig["centerTuning"] = false;
    defConfig["colorMap"] = "Classic";
    defConfig["fftHold"] = false;
    defConfig["fftHoldSpeed"] = 60;
    defConfig["fftSmoothing"] = false;
    defConfig["fftSmoothingSpeed"] = 100;
    defConfig["fastFFT"] = false;
    defConfig["fftHeight"] = 300;
    defConfig["fftRate"] = 20;
    defConfig["fftSize"] = 65536;
    defConfig["fftWindow"] = 2;
    defConfig["frequency"] = 100000000.0;
    defConfig["fullWaterfallUpdate"] = false;
    defConfig["zoomBw"] = 1.0;
    defConfig["max"] = 0.0;
    defConfig["maximized"] = false;
    defConfig["fullscreen"] = false;

    // Menu
    defConfig["menuElements"] = json::array();
    auto &menuElements  = defConfig["menuElements"];
    std::vector<std::pair<const char *, bool>> openState = {
        {"Source", true},
        {"Radio", true},
        {"Recorder", false},
        {"Sinks", false},
        {"Frequency Manager", false},
        {"VFO Color", false},
        {"Band Plan", false},
        {"Display", true},
        {"WebSDR View", false},
        {"Noise Reduction logmmse", false},
        {"FT8/FT4 Decoder", false},
        {"Rigctl Server", false},
        {"Module Manager", false},
    };
    for(auto &p: openState) {
        menuElements[menuElements.size() - 0]["name"] = p.first;
        menuElements[menuElements.size() - 1]["open"] = p.second;
    }


#ifdef __ANDROID__
    defConfig["menuWidth"] = 700;
#else
    defConfig["menuWidth"] = 300;
#endif
    defConfig["min"] = -120.0;

    // Module instances
    defConfig["moduleInstances"]["Airspy Source"]["module"] = "airspy_source";
    defConfig["moduleInstances"]["Airspy Source"]["enabled"] = true;
    defConfig["moduleInstances"]["AirspyHF+ Source"]["module"] = "airspyhf_source";
    defConfig["moduleInstances"]["AirspyHF+ Source"]["enabled"] = true;
    defConfig["moduleInstances"]["Audio Source"]["module"] = "audio_source";
    defConfig["moduleInstances"]["Audio Source"]["enabled"] = true;
    defConfig["moduleInstances"]["BladeRF Source"]["module"] = "bladerf_source";
    defConfig["moduleInstances"]["BladeRF Source"]["enabled"] = true;
    defConfig["moduleInstances"]["File Source"]["module"] = "file_source";
    defConfig["moduleInstances"]["File Source"]["enabled"] = true;
    defConfig["moduleInstances"]["HackRF Source"]["module"] = "hackrf_source";
    defConfig["moduleInstances"]["HackRF Source"]["enabled"] = true;
    defConfig["moduleInstances"]["Hermes Source"]["module"] = "hermes_source";
    defConfig["moduleInstances"]["Hermes Source"]["enabled"] = true;
    defConfig["moduleInstances"]["LimeSDR Source"]["module"] = "limesdr_source";
    defConfig["moduleInstances"]["LimeSDR Source"]["enabled"] = true;
    defConfig["moduleInstances"]["PlutoSDR Source"]["module"] = "plutosdr_source";
    defConfig["moduleInstances"]["PlutoSDR Source"]["enabled"] = true;
    defConfig["moduleInstances"]["RFspace Source"]["module"] = "rfspace_source";
    defConfig["moduleInstances"]["RFspace Source"]["enabled"] = true;
    defConfig["moduleInstances"]["RTL-SDR Source"]["module"] = "rtl_sdr_source";
    defConfig["moduleInstances"]["RTL-SDR Source"]["enabled"] = true;
    defConfig["moduleInstances"]["RTL-TCP Source"]["module"] = "rtl_tcp_source";
    defConfig["moduleInstances"]["RTL-TCP Source"]["enabled"] = true;
    defConfig["moduleInstances"]["SDRplay Source"]["module"] = "sdrplay_source";
    defConfig["moduleInstances"]["SDRplay Source"]["enabled"] = true;
    defConfig["moduleInstances"]["SDR++ Server Source"]["module"] = "sdrpp_server_source";
    defConfig["moduleInstances"]["SDR++ Server Source"]["enabled"] = true;
    defConfig["moduleInstances"]["SoapySDR Source"]["module"] = "soapy_source";
    defConfig["moduleInstances"]["SoapySDR Source"]["enabled"] = true;
    defConfig["moduleInstances"]["SpyServer Source"]["module"] = "spyserver_source";
    defConfig["moduleInstances"]["SpyServer Source"]["enabled"] = true;
    defConfig["moduleInstances"]["KiwiSDR Source"]["module"] = "kiwisdr_source";
    defConfig["moduleInstances"]["KiwiSDR Source"]["enabled"] = true;
    defConfig["moduleInstances"]["PlutoSDR Source"]["module"] = "plutosdr_source";
    defConfig["moduleInstances"]["PlutoSDR Source"]["enabled"] = true;
    defConfig["moduleInstances"]["HL2 Source"]["module"] = "hl2_source";
    defConfig["moduleInstances"]["HL2 Source"]["enabled"] = true;
    defConfig["moduleInstances"]["KiwiSDR Source"]["module"] = "kiwisdr_source";
    defConfig["moduleInstances"]["KiwiSDR Source"]["enabled"] = true;

    defConfig["moduleInstances"]["Audio Sink"] = "audio_sink";
    defConfig["moduleInstances"]["Network Sink"] = "network_sink";

    defConfig["moduleInstances"]["Radio"] = "radio";

    defConfig["moduleInstances"]["Frequency Manager"] = "frequency_manager";
    defConfig["moduleInstances"]["WebSDR View"] = "websdr_view";
    defConfig["moduleInstances"]["Recorder"] = "recorder";
    defConfig["moduleInstances"]["Rigctl Server"] = "rigctl_server";
    defConfig["moduleInstances"]["Noise Reduction logmmse"]["module"] = "noise_reduction_logmmse";
    defConfig["moduleInstances"]["Noise Reduction logmmse"]["enabled"] = true;
    defConfig["moduleInstances"]["FT8/FT4 Decoder"]["module"] = "ft8_decoder";
    defConfig["moduleInstances"]["FT8/FT4 Decoder"]["enabled"] = true;
    // defConfig["moduleInstances"]["Rigctl Client"] = "rigctl_client";
    // TODO: Enable rigctl_client when ready
    // defConfig["moduleInstances"]["Scanner"] = "scanner";
    // TODO: Enable scanner when ready


    // Themes
    defConfig["theme"] = "Dark";
#ifdef __ANDROID__
    defConfig["uiScale"] = displaymenu::displayDensity;
#else
    defConfig["uiScale"] = 1.0f;
#endif

    defConfig["modules"] = json::array();
    defConfig["offsetMode"] = (int)0; // Off
    defConfig["offset"] = 0.0;
    defConfig["showMenu"] = true;
    defConfig["showWaterfall"] = true;
    defConfig["source"] = "";
    defConfig["decimationPower"] = 0;
    defConfig["iqCorrection"] = false;
    defConfig["invertIQ"] = false;
    defConfig["operatorCallsign"] = "";
    defConfig["operatorLocation"] = "KO80";

    defConfig["streams"]["Radio"]["muted"] = false;
    defConfig["streams"]["Radio"]["sink"] = "Audio";
    defConfig["streams"]["Radio"]["volume"] = 1.0f;

    defConfig["windowSize"]["h"] = 720;
    defConfig["windowSize"]["w"] = 1280;

    defConfig["vfoOffsets"] = json::object();

    defConfig["vfoColors"]["Radio"] = "#FFFFFF";

#ifdef __ANDROID__
    defConfig["lockMenuOrder"] = true;
    defConfig["smallScreen"] = true;
#else
    defConfig["lockMenuOrder"] = false;
    defConfig["smallScreen"] = false;
#endif
    defConfig["transcieverLayout"] = 0;

#if defined(_WIN32)
    defConfig["modulesDirectory"] = "./modules";
    defConfig["resourcesDirectory"] = "./res";
#elif defined(IS_MACOS_BUNDLE)
    defConfig["modulesDirectory"] = "../Plugins";
    defConfig["resourcesDirectory"] = "../Resources";
#elif defined(__ANDROID__)
    defConfig["modulesDirectory"] = root + "/modules";
    defConfig["resourcesDirectory"] = root + "/res";
#else
    defConfig["modulesDirectory"] = INSTALL_PREFIX "/lib/sdrpp/plugins";
    defConfig["resourcesDirectory"] = INSTALL_PREFIX "/share/sdrpp";
#endif

    // Load config
    std::error_code ec;
    auto path = std::filesystem::current_path(ec);
    std::cout << "Current path = " << path << std::endl;
    flog::info("Loading config from: {} (path {})",root);
    core::configManager.setPath(root + "/config.json");
    core::configManager.load(defConfig);
    core::configManager.enableAutoSave();
    core::configManager.acquire();

    // Android can't load just any .so file. This means we have to hardcode the name of the modules
#ifdef __ANDROID__
    int modCount = 0;
    core::configManager.conf["modules"] = json::array();

    core::configManager.conf["modules"][modCount++] = "airspy_source.so";
    core::configManager.conf["modules"][modCount++] = "airspyhf_source.so";
    core::configManager.conf["modules"][modCount++] = "hackrf_source.so";
    core::configManager.conf["modules"][modCount++] = "hermes_source.so";
    core::configManager.conf["modules"][modCount++] = "plutosdr_source.so";
    core::configManager.conf["modules"][modCount++] = "rfspace_source.so";
    core::configManager.conf["modules"][modCount++] = "rtl_sdr_source.so";
    core::configManager.conf["modules"][modCount++] = "rtl_tcp_source.so";
    core::configManager.conf["modules"][modCount++] = "sdrpp_server_source.so";
    core::configManager.conf["modules"][modCount++] = "spyserver_source.so";
    core::configManager.conf["modules"][modCount++] = "kiwisdr_source.so";

    core::configManager.conf["modules"][modCount++] = "network_sink.so";
    core::configManager.conf["modules"][modCount++] = "audio_sink.so";

    core::configManager.conf["modules"][modCount++] = "m17_decoder.so";
    core::configManager.conf["modules"][modCount++] = "meteor_demodulator.so";
    core::configManager.conf["modules"][modCount++] = "radio.so";

    core::configManager.conf["modules"][modCount++] = "frequency_manager.so";
    core::configManager.conf["modules"][modCount++] = "websdr_view.so";
    core::configManager.conf["modules"][modCount++] = "recorder.so";
    core::configManager.conf["modules"][modCount++] = "rigctl_server.so";
    core::configManager.conf["modules"][modCount++] = "scanner.so";
    core::configManager.conf["modules"][modCount++] = "hl2_source.so";
    core::configManager.conf["modules"][modCount++] = "noise_reduction_logmmse.so";
    core::configManager.conf["modules"][modCount++] = "ft8_decoder.so";
#endif

    // Fix missing elements in config
    for (auto const& item : defConfig.items()) {
        if (!core::configManager.conf.contains(item.key())) {
            flog::info("Missing key in config {0}, repairing", item.key());
            core::configManager.conf[item.key()] = defConfig[item.key()];
        }
    }

    // Remove unused elements
    /*
    auto items = core::configManager.conf.items();
    flog::info("items={}",(void*)&items);
    std::vector<std::string> keysToErase;
    for (auto const& item : items) {
        if (!defConfig.contains(item.key())) {
            flog::info("Unused key in config {0}, repairing", item.key());
            keysToErase.push_back(item.key());
        }
        flog::info("ok item: {}", item.key());
    }
    for (auto const& key : keysToErase) {
        core::configManager.conf.erase(key);
    }
     */

    // Update to new module representation in config if needed
    for (auto [_name, inst] : core::configManager.conf["moduleInstances"].items()) {
        if (!inst.is_string()) { continue; }
        std::string mod = inst;
        json newMod;
        newMod["module"] = mod;
        newMod["enabled"] = true;
        core::configManager.conf["moduleInstances"][_name] = newMod;
    }

    // Load UI scaling
    style::uiScale = core::configManager.conf["uiScale"];

    core::configManager.release(true);

    if (serverMode) { return server::main(); }

    core::configManager.acquire();
    std::string resDir = core::configManager.conf["resourcesDirectory"];
    sdrppResourcesDirectory = strdup(resDir.c_str());
    json bandColors = core::configManager.conf["bandColors"];
    core::configManager.release();

    // Assert that the resource directory is absolute and check existence
    resDir = std::filesystem::absolute(resDir).string();
    if (!std::filesystem::is_directory(resDir)) {
        flog::error("Resource directory doesn't exist! Please make sure that you've configured it correctly in config.json (check readme for details)");
        return 1;
    }

    // Initialize backend
    int biRes = backend::init(resDir);
    if (biRes < 0) { return biRes; }

    // Initialize SmGui in normal mode
    SmGui::init(false);

    if (!style::loadFonts(resDir)) { return -1; }
    thememenu::init(resDir);
    LoadingScreen::init();

    LoadingScreen::show("Loading icons");
    flog::info("Loading icons");
    if (!icons::load(resDir)) { return -1; }

    LoadingScreen::show("Loading band plans");
    flog::info("Loading band plans");
    bandplan::loadFromDir(resDir + "/bandplans");

    LoadingScreen::show("Loading band plan colors");
    flog::info("Loading band plans color table");
    bandplan::loadColorTable(bandColors);

    gui::mainWindow.init();

    flog::info("Ready.");

    // Run render loop (TODO: CHECK RETURN VALUE)
    backend::renderLoop();

    gui::mainWindow.end();
    // On android, none of this shutdown should happen due to the way the UI works
#ifndef __ANDROID__
    // Shut down all modules
    for (auto& [name, mod] : core::moduleManager.modules) {
        mod.end();
    }

    // Terminate backend (TODO: CHECK RETURN VALUE)
    backend::end();

    sigpath::iqFrontEnd.stop();

    std::cout << "Save freq: " << core::configManager.conf["frequency"] << std::endl;
    core::configManager.disableAutoSave();
    core::configManager.save();
#endif

    flog::info("Exiting successfully");
    return 0;
}
