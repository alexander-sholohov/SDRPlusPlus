#pragma once
#include <dsp/block.h>
#include <volk/volk.h>
#include <utils/flog.h>
#include <dsp/types.h>
#include <dsp/processor.h>
#include "../../misc_modules/noise_reduction_logmmse/src/arrays.h"

namespace dsp {

    using namespace dsp::arrays;

    template <class T>
    class FrequencyCarving : public Processor<T, T> {
        using base = Processor<T, T>;

    public:
        FrequencyCarving() {}

        void init(stream<complex_t>* in, float inSampleRate, float outSampleRate) {
            base::init(in);
            setSampleRates(inSampleRate, outSampleRate);
        }

        std::mutex mtx;

        void setSampleRates(float inSampleRate, float outSampleRate) {
            flog::info("carving::Set sample rate: {0} {0}", inSampleRate, outSampleRate);
            std::lock_guard g(mtx);
            _inSampleRate = (int)(inSampleRate / 100);
            _outSampleRate = (int)(outSampleRate / 100);
            forward = allocateFFTWPlan(false, _inSampleRate);
            backward = allocateFFTWPlan(true, _outSampleRate);
            inputBuffer.clear();
            _inArr = dsp::npzeros_c(_inSampleRate);
        }

        std::vector<dsp::complex_t> inputBuffer;

        int process(dsp::complex_t *in, int count, dsp::complex_t *out){
            int nwritten = 0;
            auto currentInputSize = inputBuffer.size();
            inputBuffer.resize(currentInputSize + count);
            memcpy(inputBuffer.data() + currentInputSize, in, count * sizeof(dsp::complex_t));
            auto diff = (_inSampleRate - _outSampleRate);
            int cutPlace = _inSampleRate / 2 - diff / 2;
            while (inputBuffer.size() >= _inSampleRate) {
                std::copy(inputBuffer.begin(), inputBuffer.begin() + _inSampleRate, _inArr->begin());
                ComplexArray buckets = npfftfft(_inArr, forward);
                inputBuffer.erase(inputBuffer.begin(), inputBuffer.begin() + _inSampleRate);
                //                dumpArr_(buckets);
                buckets->erase(buckets->begin() + cutPlace, buckets->begin() + cutPlace + diff);
                //                dumpArr_(buckets);
                auto stream = npfftfft(buckets, backward);
                memcpy(out + nwritten, stream->data(), stream->size() * sizeof(dsp::complex_t));
                nwritten += stream->size();
            }
            return nwritten;
        }


        int run() {
            int count = this->_in->read();
            if (count < 0) {
                return -1;
            }
            auto in = this->_in->readBuf;
            std::lock_guard g(mtx);
            flog::info("carving enter");
            // TODO: Do float xlation
            if constexpr (std::is_same_v<T, float>) {
                flog::error("NOT IMPLEMENTED FOR FLOAT");
            }
            int nwritten = 0;
            if constexpr (std::is_same_v<T, complex_t>) {
                nwritten = process(in, count, this->out.writeBuf);
            }

            this->_in->flush();
            if (nwritten > 0){
                flog::info("carving swap: {0}", nwritten);
                if (!this->out.swap(nwritten)) { return -1; }
            }
            flog::info("carving out: {0}", count);
            return count;
        }


    private:
        int _inSampleRate;
        int _outSampleRate;
        Arg<fftwPlan> forward;
        Arg<fftwPlan> backward;
        ComplexArray _inArr;
    };

}