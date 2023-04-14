
#pragma once

#pragma once
#include <dsp/block.h>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <dsp/processor.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include "arrays.h"
#include "logmmse.h"

namespace dsp {

    using namespace ::dsp::arrays;
    using namespace ::dsp::logmmse;

    template <int V>
    struct SMAStream {
        std::vector<dsp::complex_t> input;
        std::vector<dsp::complex_t> output;
        void write(dsp::complex_t* values, size_t size) {
            int oldSize = input.size();
            input.resize(oldSize + size);
            memmove(input.data() + oldSize, values, size * sizeof(dsp::complex_t));
            produceSMA();
        }

        void produceSMA() {
            while (output.size() < V && input.size() > V) {
                output.emplace_back(input[output.size()]);
            }
            if (output.size() < V) {
                return;
            }
            complex_t s = { 0.0, 0.0 };
            for (int q = output.size() - V; q < output.size(); q++) {
                s += input[q];
            }
            for (int q = output.size(); q < input.size(); q++) {
                s.re -= input[q - V].re;
                s.im -= input[q - V].im;
                s.re += input[q].re;
                s.im += input[q].im;
                output.emplace_back(complex_t{ s.re / V, s.im / V });
            }
        }

        void read(dsp::complex_t* values, size_t size) {
            if (output.size() < size) {
                abort();
            }
            memmove(values, output.data(), size * sizeof(dsp::complex_t));
            output.erase(output.begin(), output.begin() + size);
            input.erase(input.begin(), input.begin() + size);
        }

        size_t available() {
            return output.size() - V;
        }
    };

    struct AFNRLogMMSE : public Processor<complex_t, complex_t> {

        using base_type = Processor<complex_t, complex_t>;

        ComplexArray worker1c;

        void init(stream<complex_t>* in) override {
            base_type::init(in);
        }

        void setInput(stream<complex_t>* in) override {
            base_type ::setInput(in);
            params.reset();
        }

        AFNRLogMMSE() {
            worker1c = std::make_shared<std::vector<complex_t>>();
        }


        LogMMSE::SavedParamsC params;

        double getVFOFrequency() {
            if (gui::waterfall.selectedVFO == "") {
                return gui::waterfall.getCenterFrequency();
            }
            else {
                return gui::waterfall.getCenterFrequency() + sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);
            }
        }

        double getVFOBandwidth() {
            if (gui::waterfall.selectedVFO == "") {
                return gui::waterfall.getBandwidth();
            }
            else {
                return sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
            }
        }

        int processingBandwidthHz = 48000/2;

        std::mutex freqMutex;

        void setProcessingBandwidth(int bandwidthHz) {
            flog::info("Refreshing noise profile for AF NR (logmmse)");
            freqMutex.lock();
            this->processingBandwidthHz = bandwidthHz;
            params.reset();
            freqMutex.unlock();
        }

        void refreshNoiseProfile() {
            flog::info("Refreshing noise profile for AF NR (logmmse)");
            freqMutex.lock();
            params.reset();
            freqMutex.unlock();
        }


        double lastVFOFrequency = 0.0;
        double lastVFOBandwidth = 0.0;
        int switchTrigger = 0;
        int overlapTrigger = -100000;

        bool allowed = false;   // initial value
        int afnrBandwidth = 10; // this is UI model value, just stored there.
        SMAStream<5> sma;
        EventHandler<bool> txHandler;

        void start() override {
            txHandler.ctx = this;
            txHandler.handler = [](bool txActive, void *ctx) {
                auto _this = (AFNRLogMMSE*)ctx;
                _this->params.hold = txActive;
            };
            block::start();
        }

        void process(complex_t *readBuf, int count, complex_t *writeBuf, int &wrote) {
            wrote = 0;
            std::lock_guard<std::mutex> lock(freqMutex);
            auto curSize = worker1c->size();
            worker1c->resize(curSize + count);
            memmove(worker1c->data() + curSize, readBuf, count * sizeof(complex_t));
            switchTrigger += count;
            overlapTrigger += count;

            int noiseFrames = 12;
            int fram = processingBandwidthHz / 100;
            auto Slen = (int)floor(0.02 * processingBandwidthHz);
            if (!params.noise_mu2) {
                if (worker1c->size() >= noiseFrames * Slen) {
                    // finally can sample
                    flog::info("Sampling, total samples: {0}, will be used: {1}", (int64_t)worker1c->size(), noiseFrames * Slen);
                    LogMMSE::logmmse_sample(worker1c, processingBandwidthHz, 0.15f, &params, noiseFrames);
                    worker1c->erase(worker1c->begin(), worker1c->begin() + curSize); // skip everything already sent to the output before
                } else {
                    // pass throug until it fills
                    memmove(writeBuf, worker1c->data() + curSize, count * sizeof(complex_t));
                    wrote = count;
                    return;
                }
            }
            int size1 = worker1c->size();
            if (worker1c->size() >= 4 * params.Slen && params.noise_mu2) {
                ALLOC_AND_CHECK(worker1c, size1, "afnr point -5")
                auto rv = LogMMSE::logmmse_all(worker1c, processingBandwidthHz, 0.15f, &params);
                int limit = rv->size();
                auto dta = rv->data();
                ALLOC_AND_CHECK(worker1c, size1, "afnr point -3")

                sma.write(dta, limit);
                ALLOC_AND_CHECK(worker1c, size1, "afnr point -2")

                if (sma.available() >= limit) {
                    wrote = limit;
                    ALLOC_AND_CHECK(worker1c, size1, "afnr point -1")
                    sma.read(writeBuf, limit);
                    ALLOC_AND_CHECK(worker1c, size1, "afnr point 0")
                    memmove(worker1c->data(), ((complex_t*)worker1c->data()) + limit, sizeof(complex_t) * (worker1c->size() - limit));
                    ALLOC_AND_CHECK(worker1c, size1, "afnr point 0.1")
                    unsigned long nsize = worker1c->size() - limit;
                    char buf[100];
                    sprintf(buf, "afnr point 0.2 size = %lld curr=%lld",(long long)nsize, (long long)worker1c->size());
                    worker1c->resize(nsize);
                    size1 = nsize;
                    ALLOC_AND_CHECK(worker1c, size1, buf)
                }
            } else {

            }
            return;
        }

        void stop() override {
            block::stop();
            sigpath::txState.unbindHandler(&txHandler);
        }

        int run() override {

            if (getVFOFrequency() != lastVFOFrequency) {
                lastVFOFrequency = getVFOFrequency();
                refreshNoiseProfile();
            }
            if (getVFOBandwidth() != lastVFOBandwidth) {
                lastVFOBandwidth = getVFOBandwidth();
                refreshNoiseProfile();
            }

            int count = _in->read();
            if (count < 0) { return -1; }

            int wrote;
            process(_in->readBuf, count, out.writeBuf, wrote);
            _in->flush();
            flog::info("afnr.mmse: input = {}, output = {}", count, wrote);
            if (!out.swap(wrote)) {
                flog::info("afnr.mmse: swap failed");
                return 0;
            }

            return 1;
        }
    };


}