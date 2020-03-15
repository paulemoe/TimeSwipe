#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <boost/lockfree/spsc_queue.hpp>
#include <future>
#include <iostream>
#include <list>
#include "timeswipe.hpp"
#include "reader.hpp"
#include "timeswipe_eeprom.hpp"
#include "timeswipe_resampler.hpp"
#include "pidfile.hpp"

bool TimeSwipe::resample_log = false;

class TimeSwipeImpl {
    static std::mutex startStopMtx;
    static TimeSwipeImpl* startedInstance;
    static const int constexpr BASE_SAMPLE_RATE = 48000;
public:
    TimeSwipeImpl();
    ~TimeSwipeImpl();
    void SetMode(int number);

    void SetSensorOffsets(int offset1, int offset2, int offset3, int offset4);
    void SetSensorGains(float gain1, float gain2, float gain3, float gain4);
    void SetSensorTransmissions(float trans1, float trans2, float trans3, float trans4);

    bool SetSampleRate(int rate);
    bool Start(TimeSwipe::ReadCallback);
    bool onButton(TimeSwipe::OnButtonCallback cb);
    bool onError(TimeSwipe::OnErrorCallback cb);
    std::string Settings(uint8_t set_or_get, const std::string& request, std::string& error);
    bool Stop();

    void SetBurstSize(size_t burst);

private:
    bool _isStarted();
    void _fetcherLoop();
    void _pollerLoop(TimeSwipe::ReadCallback cb);
    void _spiLoop();
    void _receiveEvents();

    RecordReader Rec;
    // 32 - minimal sample 48K maximal rate, next buffer is enough too keep records for 1 sec
    static const unsigned constexpr BUFFER_SIZE = 48000/32*2;
    boost::lockfree::spsc_queue<std::vector<Record>, boost::lockfree::capacity<BUFFER_SIZE>> recordBuffer;
    std::atomic_uint64_t recordErrors = 0;

    std::vector<Record> burstBuffer;
    size_t burstSize = 0;

    boost::lockfree::spsc_queue<std::pair<uint8_t,std::string>, boost::lockfree::capacity<1024>> _inSPI;
    boost::lockfree::spsc_queue<std::pair<std::string,std::string>, boost::lockfree::capacity<1024>> _outSPI;
    boost::lockfree::spsc_queue<BoardEvents, boost::lockfree::capacity<128>> _events;
    void _processSPIRequests();

    TimeSwipe::OnButtonCallback onButtonCb;
    TimeSwipe::OnErrorCallback onErrorCb;

    bool _work = false;
    std::list<std::thread> _serviceThreads;

    std::unique_ptr<TimeSwipeResampler> resampler;

    PidFile pidfile;
};

std::mutex TimeSwipeImpl::startStopMtx;
TimeSwipeImpl* TimeSwipeImpl::startedInstance = nullptr;

TimeSwipeImpl::TimeSwipeImpl()
  : pidfile("timeswipe") {
}

TimeSwipeImpl::~TimeSwipeImpl() {
    Stop();
}

void TimeSwipeImpl::SetMode(int number) {
    Rec.mode = number;
}

void TimeSwipeImpl::SetSensorOffsets(int offset1, int offset2, int offset3, int offset4) {
    Rec.offset[0] = offset1;
    Rec.offset[1] = offset2;
    Rec.offset[2] = offset3;
    Rec.offset[3] = offset4;
}

void TimeSwipeImpl::SetSensorGains(float gain1, float gain2, float gain3, float gain4) {
    Rec.gain[0] = 1.0 / gain1;
    Rec.gain[1] = 1.0 / gain2;
    Rec.gain[2] = 1.0 / gain3;
    Rec.gain[3] = 1.0 / gain4;
}

void TimeSwipeImpl::SetSensorTransmissions(float trans1, float trans2, float trans3, float trans4) {
    Rec.transmission[0] = 1.0 / trans1;
    Rec.transmission[1] = 1.0 / trans2;
    Rec.transmission[2] = 1.0 / trans3;
    Rec.transmission[3] = 1.0 / trans4;
}


bool TimeSwipeImpl::SetSampleRate(int rate) {
    if (rate < 1 || rate > BASE_SAMPLE_RATE) return false;
    resampler.reset(nullptr);
    if (rate != BASE_SAMPLE_RATE)
        resampler = std::make_unique<TimeSwipeResampler>(rate, BASE_SAMPLE_RATE);
    return true;
}

bool TimeSwipeImpl::Start(TimeSwipe::ReadCallback cb) {
    {
        std::lock_guard<std::mutex> lock(startStopMtx);
        if (_work || startedInstance) {
            return false;
        }
        std::string err;
        // lock at the start
        // second locke from the same instance is allowed and returns success
        if (!pidfile.Lock(err)) {
            std::cerr << "pid file lock failed: \"" << err << "\"" << std::endl;
            return false;
        }
        startedInstance = this;
        if (!TimeSwipeEEPROM::Read(err)) {
            std::cerr << "EEPROM read failed: \"" << err << "\"" << std::endl;
            //TODO: uncomment once parsing implemented
            //return false;
        }
    }

    Rec.setup();
    Rec.start();

    _work = true;
    _serviceThreads.push_back(std::thread(std::bind(&TimeSwipeImpl::_fetcherLoop, this)));
    _serviceThreads.push_back(std::thread(std::bind(&TimeSwipeImpl::_pollerLoop, this, cb)));
    _serviceThreads.push_back(std::thread(std::bind(&TimeSwipeImpl::_spiLoop, this)));

    return true;
}

bool TimeSwipeImpl::Stop() {
    {
        std::lock_guard<std::mutex> lock(startStopMtx);
        if (!_work || startedInstance != this) {
            return false;
        }
        startedInstance = nullptr;
    }

    _work = false;

    for (auto& th: _serviceThreads)
        if(th.joinable())
            th.join();

    while (recordBuffer.pop());
    while (_inSPI.pop());
    while (_outSPI.pop());

    Rec.stop();

    return true;
}

bool TimeSwipeImpl::onButton(TimeSwipe::OnButtonCallback cb) {
    if (_isStarted()) return false;
    onButtonCb = cb;
    return true;
}

bool TimeSwipeImpl::onError(TimeSwipe::OnErrorCallback cb) {
    if (_isStarted()) return false;
    onErrorCb = cb;
    return true;
}

std::string TimeSwipeImpl::Settings(uint8_t set_or_get, const std::string& request, std::string& error) {
    _inSPI.push(std::make_pair(set_or_get, request));
    std::pair<std::string,std::string> resp;

    if (!_work) {
        _processSPIRequests();
    }

    while (!_outSPI.pop(resp)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    error = resp.second;

    return resp.first;
}

bool TimeSwipeImpl::_isStarted() {
    std::lock_guard<std::mutex> lock(startStopMtx);
    return startedInstance != nullptr;
}

void TimeSwipeImpl::_receiveEvents() {
    auto event = readBoardEvents();
    if (event.button) {
        _events.push(event);
    }
}

void TimeSwipeImpl::_processSPIRequests() {
    std::pair<uint8_t,std::string> request;
    while (_inSPI.pop(request)) {
        std::string error;
        auto response = request.first ? readBoardSetSettings(request.second, error) : readBoardGetSettings(request.second, error);
        _outSPI.push(std::make_pair(response, error));
    }
}

TimeSwipe::TimeSwipe() {
    _impl = std::make_unique<TimeSwipeImpl>();
}

TimeSwipe::~TimeSwipe() {
    Stop();
}

void TimeSwipe::SetSensorOffsets(int offset1, int offset2, int offset3, int offset4) {
    return _impl->SetSensorOffsets(offset1, offset2, offset3, offset4);
}

void TimeSwipe::SetSensorGains(float gain1, float gain2, float gain3, float gain4) {
    return _impl->SetSensorGains(gain1, gain2, gain3, gain4);
}

void TimeSwipe::SetSensorTransmissions(float trans1, float trans2, float trans3, float trans4) {
    return _impl->SetSensorTransmissions(trans1, trans2, trans3, trans4);
}

void TimeSwipe::SetMode(Mode number) {
    return _impl->SetMode(int(number));
}

//TODO:
/*
Mode TimeSwipe::GetMode() {
    return TimeSwipe::Mode(_impl->GetMode());
}
*/

void TimeSwipe::SetBurstSize(size_t burst) {
    return _impl->SetBurstSize(burst);
}

bool TimeSwipe::SetSampleRate(int rate) {
    return _impl->SetSampleRate(rate);
}

bool TimeSwipe::Start(TimeSwipe::ReadCallback cb) {
    return _impl->Start(cb);
}

bool TimeSwipe::onError(TimeSwipe::OnErrorCallback cb) {
    return _impl->onError(cb);
}

bool TimeSwipe::onEvent(TimeSwipe::OnButtonCallback cb) {
    return _impl->onButton(cb);
}

std::string TimeSwipe::SetSettings(const std::string& request, std::string& error) {
    return _impl->Settings(1, request, error);
}

std::string TimeSwipe::GetSettings(const std::string& request, std::string& error) {
    return _impl->Settings(0, request, error);
}

void TimeSwipeImpl::_fetcherLoop() {
    while (_work) {
        auto data = Rec.read();
        if (!recordBuffer.push(data))
            ++recordErrors;

        BoardEvents event;
        while (_events.pop(event)) {
            if (event.button && onButtonCb) {
                onButtonCb(event.buttonCounter % 2, event.buttonCounter);
            }
        }
    }
}

void TimeSwipeImpl::_spiLoop() {
    while (_work) {
        _receiveEvents();
        _processSPIRequests();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void TimeSwipeImpl::_pollerLoop(TimeSwipe::ReadCallback cb) {
    while (_work)
    {
        std::vector<Record> empty;
        std::vector<Record> records[4096];
        auto num = recordBuffer.pop(&records[0], 4096);
        uint64_t errors = recordErrors.fetch_and(0UL);
        if (num == 0 && errors == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        if (errors && onErrorCb) onErrorCb(errors);

        std::vector<Record>* records_ptr = nullptr;
        std::vector<Record> samples;
        if (resampler) {
            for (size_t i = 0; i < num; i++) {
                auto s = resampler->Resample(std::move(records[i]));
                std::move(s.begin(), s.end(), std::back_inserter(samples));
            }
            records_ptr = &samples;
        } else {
            for (size_t i = 1; i < num; i++) {
                std::move(records[i].begin(), records[i].end(), std::back_inserter(records[0]));
            }
            records_ptr = &records[0];
        }

        if (burstBuffer.empty() && burstSize <= records_ptr->size()) {
            // optimization if burst buffer not used or smaller than first buffer
            cb(std::move(*records_ptr), errors);
            records_ptr->clear();
        } else {
            // burst buffer mode
            std::move(records_ptr->begin(), records_ptr->end(), std::back_inserter(burstBuffer));
            records_ptr->clear();
            if (burstBuffer.size() >= burstSize) {
                cb(std::move(burstBuffer), errors);
                burstBuffer.clear();
            }
        }
    }
}

void TimeSwipeImpl::SetBurstSize(size_t burst) {
    burstSize = burst;
}

bool TimeSwipe::Stop() {
    return _impl->Stop();
}

