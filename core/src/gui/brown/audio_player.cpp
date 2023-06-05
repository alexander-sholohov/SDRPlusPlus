
#include "audio_player.h"
#include <utils/wav.h>
#include "gui/gui.h"
#include <signal_path/signal_path.h>
#include <gui/widgets/finger_button.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())
#define CONCAT2(a, b, c) ((std::string(a) + b + c).c_str())

void AudioPlayer::draw() {
    if (error != "") {
        ImGui::TextUnformatted("Error: ");
        ImGui::SameLine();
        ImGui::TextUnformatted(error.c_str());
    } else {
        ImGui::TextUnformatted("Seek: ");
        ImGui::SameLine();
        float end = 0.0;
        if (data->size() > 0) {
            end = data->size() / (float)sampleRate;
        }
        char formatt[100];
        sprintf(formatt, "%%.1f / %.1f seconds", end);
        if (ImGui::SliderFloat("", &position, 0, end, formatt)) {
            dataPosition = position * sampleRate;
            if (dataPosition >= data->size()) {
                dataPosition = (int64_t)data->size() - 4096;
                if (dataPosition < 0) {
                    dataPosition = 0;
                }
            }
        }

        if (playing) {
            if (doFingerButton("STOP")) {
                playing = false;
            }
        } else {
            if (doFingerButton("PLAY")) {
                playing = true;
                startPlaying();
            }
        }

    }

}


void AudioPlayer::startPlaying() {
    if (!data->empty()) {
        auto radioName = gui::waterfall.selectedVFO;
        if (!radioName.empty()) {
            playing = true;
            std::thread x([this] {
                dsp::routing::Merger<dsp::stereo_t>* merger = sigpath::sinkManager.getMerger(gui::waterfall.selectedVFO);
                outStream.clearReadStop();
                outStream.clearWriteStop();
                merger->bindStream(-10, &outStream);
                auto waitTil = (double)currentTimeMillis();
                for (dataPosition = 0; dataPosition < data->size() && playing; dataPosition += 1024) {
                    auto ctm = currentTimeMillis();
                    if (ctm < (long long)waitTil) {
                        std::this_thread::sleep_for(std::chrono::milliseconds((int64_t)waitTil - ctm));
                    }
                    size_t blockEnd = dataPosition + 1024;
                    if (blockEnd > data->size()) {
                        blockEnd = data->size();
                    }
                    std::copy(data->data() + dataPosition, data->data() + blockEnd, outStream.writeBuf);
                    waitTil += 1000 * (blockEnd - dataPosition) / sampleRate;
                    //                        flog::info("player Swapping to {}: {0} - {1} = {}, wait til {}", audioOut.origin, blockEnd, i, blockEnd - i, (int64_t)waitTil);
                    outStream.swap(blockEnd - dataPosition);
                    position = dataPosition / (float)sampleRate;
                    //                        flog::info("player Swapped to {}: {} - {} = {}", audioOut.origin, blockEnd, i, blockEnd - i);
                }
                flog::info("startPlaying: stop");
                merger->unbindStream(&outStream);
                playing = false;
            });
            x.detach();
        }
    }

}
void AudioPlayer::loadFile(const std::string& path) {
    this->data = &this->dataOwn;
    wav::Reader reader(path);
    if (!reader.isValid()) {
        error = "WAV read error";
    } else {
        this->sampleRate = reader.getSampleRate();
        std::vector<dsp::stereo_t> buffer;
        const int BUFSIZE = 20000;
        buffer.reserve(BUFSIZE);
        this->data->clear();
        while (true) {
            auto reqsize = BUFSIZE * sizeof(buffer[0]);
            auto rd = reader.readSamples2(buffer.data(), reqsize);
            buffer.resize(rd / sizeof(buffer[0]));
            data->insert(data->end(), buffer.begin(), buffer.end());
            if (rd != reqsize) {
                break;
            }
        }
    }

}
void AudioPlayer::setData(std::vector<dsp::stereo_t>* data, uint32_t sampleRate) {
    this->data = data;
    this->sampleRate = sampleRate;
}
