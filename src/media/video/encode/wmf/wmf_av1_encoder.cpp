#include "wmf_av1_encoder.h"

#include <codecapi.h>
#include <strmif.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

#include "avt/svt_av1_encoder.h"
#include "encoder_input_timing.h"
#include "log.h"
#include "obu_parser.h"
#include "wmf/wmf_utils.h"

namespace minirtc {
namespace {
using wmf::ComPtr;

// MFVideoFormat_AV1 = {31305641-0000-0010-8000-00AA00389B71} (FOURCC 'AV01').
// Defined explicitly so compilation does not depend on Windows SDK sub-version.
constexpr GUID kMediaSubTypeAv1 = {
    0x31305641,
    0x0000,
    0x0010,
    {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

HRESULT SetUint(ICodecAPI* api, const GUID& key, ULONG value) {
  VARIANT variant{};
  variant.vt = VT_UI4;
  variant.ulVal = value;
  return api->SetValue(&key, &variant);
}

HRESULT EnumerateEncoders(IMFActivate*** activates, UINT32* count) {
  MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, kMediaSubTypeAv1};
  return MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                   MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &input,
                   &output, activates, count);
}

int EffectiveTargetBitrate(const MediaCodecConfig& config) {
  const int base =
      config.init_bitrate > 0 ? config.init_bitrate : config.average_bitrate;
  return ClampEncoderTargetBitrate(base, config.max_bitrate);
}

int ComputePeakBitrate(int target_bps, int max_bps) {
  // Keep PeakConstrainedVBR bursts smooth (1.20x) so keyframes and fast
  // scrolling do not overflow shallow cellular/TURN UDP queues.
  const int64_t burst = static_cast<int64_t>(target_bps) * 6 / 5;
  const int64_t cap =
      max_bps > 0 ? std::max<int64_t>(target_bps, max_bps) : burst;
  const int64_t clamped = std::min<int64_t>(
      std::min<int64_t>(burst, cap), std::numeric_limits<int>::max());
  return static_cast<int>(std::max<int64_t>(target_bps, clamped));
}

ULONG SelectQualityVsSpeed(const MediaCodecConfig& config,
                           bool speed_priority = false) {
  if (speed_priority ||
      config.video_degradation_preference ==
          MiniRtcVideoDegradationPreference::MaintainFrameRate) {
    return 0;
  }
  // At 60 fps (16.6 ms budget) or >4.2M px, QvS=25 keeps Intel Arc AV1 encode
  // well within budget while preserving sharp desktop text.
  const int64_t pixels =
      static_cast<int64_t>(config.init_width) * config.init_height;
  if (config.max_frame_rate > 30 || pixels > 4200000) {
    return 25;
  }
  return 50;
}

bool ValidConfig(const MediaCodecConfig& config) {
  size_t size = 0;
  return config.init_width > 0 && config.init_height > 0 &&
         config.init_width <= 8192 && config.init_height <= 8192 &&
         GetNv12FrameSize(config.init_width, config.init_height, &size) &&
         (config.init_bitrate > 0 || config.average_bitrate > 0) &&
         config.max_bitrate >= 0 && config.max_frame_rate > 0 &&
         config.max_frame_rate <= 240 && config.key_frame_interval > 0;
}

struct AsyncEventQueue {
  std::mutex mutex;
  std::condition_variable cv;
  std::queue<std::pair<HRESULT, ComPtr<IMFMediaEvent>>> items;
  bool pending = false;
};

class MftEventCallback final : public IMFAsyncCallback {
 public:
  MftEventCallback(ComPtr<IMFMediaEventGenerator> gen,
                   std::shared_ptr<AsyncEventQueue> queue)
      : gen_(std::move(gen)), queue_(std::move(queue)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
      *ppv = static_cast<IMFAsyncCallback*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(
        ref_count_.fetch_add(1, std::memory_order_relaxed) + 1);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG count = static_cast<ULONG>(
        ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1);
    if (count == 0) delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE GetParameters(DWORD* flags, DWORD* queue) override {
    if (flags) *flags = 0;
    if (queue) *queue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult* result) override {
    ComPtr<IMFMediaEvent> event;
    const HRESULT hr = gen_->EndGetEvent(result, &event);
    {
      std::lock_guard<std::mutex> lock(queue_->mutex);
      queue_->pending = false;
      queue_->items.emplace(hr, std::move(event));
    }
    queue_->cv.notify_one();
    return S_OK;
  }

 private:
  std::atomic<long> ref_count_{1};
  ComPtr<IMFMediaEventGenerator> gen_;
  std::shared_ptr<AsyncEventQueue> queue_;
};
}  // namespace

class WmfAv1Encoder::Impl {
 public:
  explicit Impl(std::shared_ptr<SystemClock> clock)
      : clock_(std::move(clock)) {}
  ~Impl() {
    worker_.Call([this] {
      Reset();
      fallback_.reset();
    });
  }

  void Reset() {
    event_callback_.Reset();
    event_queue_.reset();
    events_.Reset();
    api_.Reset();
    wmf::ShutdownTransform(encoder_);
    input_credit_ = 0;
    sequence_header_.clear();
    sequence_ = 0;
    peak_constrained_vbr_ = false;
    applied_bitrate_ = 0;
    missed_idr_frames_ = 0;
    force_idr_ = true;
    sample_clock_.Reset();
  }

  HRESULT Configure(IMFActivate* activation) {
    HRESULT hr = activation->ActivateObject(IID_PPV_ARGS(&encoder_));
    if (FAILED(hr)) return hr;
    ComPtr<IMFAttributes> attributes;
    hr = encoder_->GetAttributes(&attributes);
    if (FAILED(hr)) return hr;
    async_ =
        MFGetAttributeUINT32(attributes.Get(), MF_TRANSFORM_ASYNC, FALSE) != 0;
    if (async_) {
      hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
      if (FAILED(hr)) return hr;
      hr = encoder_.As(&events_);
      if (FAILED(hr)) return hr;
      event_queue_ = std::make_shared<AsyncEventQueue>();
      event_callback_.Attach(new MftEventCallback(events_, event_queue_));
    }
    DWORD inputs = 0, outputs = 0;
    hr = encoder_->GetStreamCount(&inputs, &outputs);
    if (FAILED(hr) || inputs != 1 || outputs != 1) return E_UNEXPECTED;
    input_id_ = output_id_ = 0;
    hr = encoder_->GetStreamIDs(1, &input_id_, 1, &output_id_);
    if (FAILED(hr) && hr != E_NOTIMPL) return hr;
    hr = encoder_.As(&api_);
    if (FAILED(hr)) return hr;

    // Prefer PeakConstrainedVBR: on Intel Hardware AV1 MFT, CBR ignores
    // dynamic MeanBitRate updates (returns S_FALSE) whereas PeakConstrainedVBR
    // supports both dynamic MeanBitRate and MaxBitRate updates live.
    // Do NOT override CODECAPI_AVEncCommonBufferSize: doing so causes Intel
    // AV1 MFT ProcessOutput to stall with zero output frames.
    hr = SetUint(api_.Get(), CODECAPI_AVEncCommonRateControlMode,
                 eAVEncCommonRateControlMode_PeakConstrainedVBR);
    if (hr == S_OK) {
      peak_constrained_vbr_ = true;
    } else {
      peak_constrained_vbr_ = false;
      hr = SetUint(api_.Get(), CODECAPI_AVEncCommonRateControlMode,
                   eAVEncCommonRateControlMode_CBR);
      if (FAILED(hr)) return hr;
    }

    VARIANT low_latency{};
    low_latency.vt = VT_BOOL;
    low_latency.boolVal = VARIANT_TRUE;
    api_->SetValue(&CODECAPI_AVLowLatencyMode, &low_latency);
    SetUint(api_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    SetUint(api_.Get(), CODECAPI_AVEncMPVGOPSize, config_.key_frame_interval);
    applied_qvs_ = SelectQualityVsSpeed(config_, speed_priority_);
    SetUint(api_.Get(), CODECAPI_AVEncCommonQualityVsSpeed, applied_qvs_);

    ComPtr<IMFMediaType> output, input;
    hr = MFCreateMediaType(&output);
    if (FAILED(hr)) return hr;
    output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output->SetGUID(MF_MT_SUBTYPE, kMediaSubTypeAv1);
    output->SetUINT32(MF_MT_AVG_BITRATE, config_.init_bitrate);
    output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, config_.init_width,
                       config_.init_height);
    MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, config_.max_frame_rate,
                        1);
    MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = encoder_->SetOutputType(output_id_, output.Get(), 0);
    if (FAILED(hr)) return hr;

    hr = MFCreateMediaType(&input);
    if (FAILED(hr)) return hr;
    output->CopyAllItems(input.Get());
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    input->DeleteItem(MF_MT_AVG_BITRATE);
    input->SetUINT32(MF_MT_DEFAULT_STRIDE, config_.init_width);
    hr = encoder_->SetInputType(input_id_, input.Get(), 0);
    if (FAILED(hr)) return hr;

    hr = SetUint(api_.Get(), CODECAPI_AVEncCommonMeanBitRate,
                 config_.init_bitrate);
    if (FAILED(hr)) return hr;
    applied_bitrate_ = config_.init_bitrate;
    if (peak_constrained_vbr_) {
      SetUint(api_.Get(), CODECAPI_AVEncCommonMaxBitRate,
              ComputePeakBitrate(config_.init_bitrate, config_.max_bitrate));
    }

    hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) return hr;
    hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) return hr;
    return S_OK;
  }

  HRESULT Initialize() {
    Reset();
    if (FAILED(worker_.Status())) return worker_.Status();
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    HRESULT hr = EnumerateEncoders(&activations, &count);
    if (FAILED(hr)) return hr;
    hr = MF_E_TOPO_CODEC_NOT_FOUND;
    for (UINT32 i = 0; i < count; ++i) {
      if (!encoder_) {
        hr = Configure(activations[i]);
        if (FAILED(hr)) {
          LOG_DEBUG("Hardware AV1 MFT initialization failed: 0x{:08x}",
                    static_cast<uint32_t>(hr));
          Reset();
          activations[i]->ShutdownObject();
        } else {
          WCHAR name[256]{};
          activations[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256,
                                    nullptr);
          char utf8[1024]{};
          WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr,
                              nullptr);
          LOG_INFO(
              "Media Foundation hardware AV1 encoder: {} (rc={}, qvs={}, "
              "{}x{} @ {}fps, target={} bps, max={} bps)",
              utf8, peak_constrained_vbr_ ? "PeakConstrainedVBR" : "CBR",
              applied_qvs_, config_.init_width,
              config_.init_height, config_.max_frame_rate, config_.init_bitrate,
              config_.max_bitrate);
          activations[i]->DetachObject();
        }
      }
      activations[i]->Release();
    }
    CoTaskMemFree(activations);
    reconfigure_ = false;
    return encoder_ ? S_OK : hr;
  }

  HRESULT ReadFrame(IMFSample* sample, const RawFrame& raw,
                    std::vector<EncodedFrame>& frames) {
    std::vector<uint8_t> bytes;
    HRESULT hr = wmf::ReadBytes(sample, bytes);
    if (FAILED(hr)) return hr;
    if (bytes.empty()) return MF_E_INVALID_STREAM_DATA;

    const auto obus =
        obu::ParseObus(bytes.data(), static_cast<int>(bytes.size()));
    if (obus.empty()) return MF_E_INVALID_STREAM_DATA;

    bool has_sequence_header = false;
    for (const auto& o : obus) {
      if (obu::ObuType(o.header) == obu::OBU_SEQUENCE_HEADER &&
          !o.payload.empty()) {
        sequence_header_ = o.payload;
        has_sequence_header = true;
      }
    }

    const bool clean_point =
        MFGetAttributeUINT32(sample, MFSampleExtension_CleanPoint, FALSE) != 0;
    const bool key = clean_point || has_sequence_header;

    if (force_idr_ && !key) {
      if (sequence_ == 0 || ++missed_idr_frames_ > 2)
        return MF_E_INVALID_STREAM_DATA;
    } else if (key) {
      force_idr_ = false;
      missed_idr_frames_ = 0;
      if (!has_sequence_header) {
        if (sequence_header_.empty()) return MF_E_INVALID_STREAM_DATA;
        // Insert cached sequence header after any leading temporal delimiter.
        size_t insert_pos = 0;
        if (bytes.size() >= 2 && bytes[0] == 0x12 && bytes[1] == 0x00) {
          insert_pos = 2;
        }
        bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(insert_pos),
                     sequence_header_.begin(), sequence_header_.end());
      }
    }

    EncodedFrame frame(bytes.data(), bytes.size(), raw.Width(), raw.Height());
    frame.SetEncodedWidth(raw.Width());
    frame.SetEncodedHeight(raw.Height());
    frame.SetFrameType(key ? kVideoFrameKey : kVideoFrameDelta);
    frame.SetCapturedTimestamp(raw.CapturedTimestamp());
    frame.SetEncodedTimestamp(clock_->CurrentTime());
    frames.push_back(std::move(frame));
    return S_OK;
  }

  HRESULT EncodeHardware(const RawFrame& raw,
                         std::vector<EncodedFrame>& frames) {
    if (raw.Width() != static_cast<uint32_t>(config_.init_width) ||
        raw.Height() != static_cast<uint32_t>(config_.init_height) ||
        reconfigure_) {
      config_.init_width = static_cast<int>(raw.Width());
      config_.init_height = static_cast<int>(raw.Height());
      HRESULT hr = Initialize();
      if (FAILED(hr)) return hr;
    }
    if (!encoder_) return MF_E_NOT_INITIALIZED;
    if (sequence_ % config_.key_frame_interval == 0) force_idr_ = true;
    if (force_idr_) {
      HRESULT hr = SetUint(api_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
      if (FAILED(hr)) return hr;
    }

    ComPtr<IMFSample> input;
    const auto timing = sample_clock_.Next(
        raw.CapturedTimestamp(), clock_->CurrentTimeUs(), input_frame_rate_);
    size_t input_size = 0;
    GetNv12FrameSize(raw.Width(), raw.Height(), &input_size);
    HRESULT hr = wmf::CreateSample(raw.Buffer(), input_size, timing.time_100ns,
                                   timing.duration_100ns, input);
    if (FAILED(hr)) return hr;

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool submitted = false;
    while (std::chrono::steady_clock::now() < deadline) {
      if (!submitted && (!async_ || input_credit_ > 0)) {
        hr = encoder_->ProcessInput(input_id_, input.Get(), 0);
        if (SUCCEEDED(hr)) {
          submitted = true;
          if (async_) --input_credit_;
        } else if (hr != MF_E_NOTACCEPTING) {
          return hr;
        }
      }
      bool have_output = !async_;
      if (async_) {
        ComPtr<IMFMediaEvent> event;
        hr = NextAsyncEvent(deadline, event);
        if (FAILED(hr)) return hr;
        HRESULT status = S_OK;
        hr = event->GetStatus(&status);
        if (FAILED(hr)) return hr;
        if (FAILED(status)) return status;
        MediaEventType type = MEUnknown;
        hr = event->GetType(&type);
        if (FAILED(hr)) return hr;
        if (type == METransformNeedInput) ++input_credit_;
        if (type == METransformHaveOutput) have_output = true;
      }
      if (have_output) {
        ComPtr<IMFSample> output;
        hr = wmf::ReadOutput(encoder_.Get(), output_id_, output);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
          ComPtr<IMFMediaType> type;
          hr = encoder_->GetOutputAvailableType(output_id_, 0, &type);
          if (FAILED(hr)) return hr;
          hr = encoder_->SetOutputType(output_id_, type.Get(), 0);
          if (FAILED(hr)) return hr;
          if (async_) continue;
          hr = wmf::ReadOutput(encoder_.Get(), output_id_, output);
        }
        if (FAILED(hr) && hr != MF_E_TRANSFORM_NEED_MORE_INPUT) return hr;
        if (SUCCEEDED(hr) && output) {
          hr = ReadFrame(output.Get(), raw, frames);
          if (FAILED(hr)) return hr;
          ++sequence_;
          return S_OK;
        }
      }
    }
    return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
  }

  wmf::CodecThread worker_;
  std::shared_ptr<SystemClock> clock_;
  MediaCodecConfig config_;
  HRESULT NextAsyncEvent(std::chrono::steady_clock::time_point deadline,
                         ComPtr<IMFMediaEvent>& event_out) {
    if (!events_ || !event_queue_ || !event_callback_) return E_UNEXPECTED;
    for (;;) {
      bool start_get_event = false;
      {
        std::unique_lock<std::mutex> lock(event_queue_->mutex);
        if (!event_queue_->items.empty()) {
          auto item = std::move(event_queue_->items.front());
          event_queue_->items.pop();
          event_out = std::move(item.second);
          return item.first;
        }
        if (!event_queue_->pending) {
          event_queue_->pending = true;
          start_get_event = true;
        } else {
          if (!event_queue_->cv.wait_until(
                  lock, deadline,
                  [&] { return !event_queue_->items.empty(); })) {
            return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
          }
          continue;
        }
      }
      if (start_get_event) {
        const HRESULT hr =
            events_->BeginGetEvent(event_callback_.Get(), nullptr);
        if (FAILED(hr)) {
          std::lock_guard<std::mutex> lock(event_queue_->mutex);
          event_queue_->pending = false;
          return hr;
        }
      }
    }
  }

  ComPtr<IMFTransform> encoder_;
  ComPtr<IMFMediaEventGenerator> events_;
  std::shared_ptr<AsyncEventQueue> event_queue_;
  ComPtr<IMFAsyncCallback> event_callback_;
  ComPtr<ICodecAPI> api_;
  DWORD input_id_ = 0, output_id_ = 0;
  unsigned input_credit_ = 0;
  bool async_ = false;
  bool peak_constrained_vbr_ = false;
  bool force_idr_ = true;
  bool reconfigure_ = false;
  bool initialized_ = false;
  int applied_bitrate_ = 0;
  ULONG applied_qvs_ = 25;
  bool speed_priority_ = false;
  int missed_idr_frames_ = 0;
  uint64_t sequence_ = 0;
  int input_frame_rate_ = 60;
  EncoderSampleClock sample_clock_;
  std::vector<uint8_t> sequence_header_;
  std::unique_ptr<SvtAv1Encoder> fallback_;
};

WmfAv1Encoder::WmfAv1Encoder(std::shared_ptr<SystemClock> clock)
    : impl_(std::make_unique<Impl>(std::move(clock))) {}
WmfAv1Encoder::~WmfAv1Encoder() = default;

bool WmfAv1Encoder::IsSupported() {
  wmf::CodecThread worker;
  if (FAILED(worker.Status())) return false;
  return worker.Call([] {
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    const HRESULT hr = EnumerateEncoders(&activations, &count);
    for (UINT32 i = 0; i < count; ++i) activations[i]->Release();
    CoTaskMemFree(activations);
    return SUCCEEDED(hr) && count > 0;
  });
}

int WmfAv1Encoder::Init(const MediaCodecConfig& config) {
  if (!ValidConfig(config)) return -1;
  return impl_->worker_.Call([&] {
    impl_->fallback_.reset();
    impl_->config_ = config;
    impl_->speed_priority_ = false;
    impl_->input_frame_rate_ = config.max_frame_rate;
    const int target_bps = EffectiveTargetBitrate(config);
    impl_->config_.init_bitrate = target_bps;
    impl_->config_.average_bitrate = target_bps;
    impl_->initialized_ = SUCCEEDED(impl_->Initialize());
    return impl_->initialized_ ? 0 : -1;
  });
}

int WmfAv1Encoder::Encode(const RawFrame& raw,
                          std::function<int(const EncodedFrame&)> callback) {
  size_t size = 0;
  if (!raw.Buffer() || raw.Width() > 8192 || raw.Height() > 8192 ||
      !GetNv12FrameSize(raw.Width(), raw.Height(), &size) || raw.Size() < size)
    return -1;
  std::vector<EncodedFrame> frames;
  const int result = impl_->worker_.Call([&] {
    if (!impl_->initialized_) return -1;
    if (!impl_->fallback_) {
      const HRESULT hr = impl_->EncodeHardware(raw, frames);
      if (SUCCEEDED(hr)) return 0;
      LOG_WARN(
          "Media Foundation hardware AV1 encoding failed (0x{:08x}); using "
          "SVT-AV1",
          static_cast<uint32_t>(hr));
      impl_->Reset();
      impl_->fallback_ = std::make_unique<SvtAv1Encoder>(impl_->clock_);
      MediaCodecConfig sw_config = impl_->config_;
      sw_config.init_width = static_cast<int>(raw.Width());
      sw_config.init_height = static_cast<int>(raw.Height());
      sw_config.average_bitrate = impl_->config_.init_bitrate;
      if (impl_->fallback_->Init(sw_config) != 0) {
        impl_->fallback_.reset();
        return -1;
      }
      impl_->fallback_->SetTargetFrameRate(impl_->input_frame_rate_);
    }
    return impl_->fallback_->Encode(raw, [&](const EncodedFrame& frame) {
      frames.push_back(frame);
      return 0;
    });
  });
  for (const auto& frame : frames)
    if (callback) callback(frame);
  return result;
}

int WmfAv1Encoder::ForceIdr() {
  return impl_->worker_.Call([&] {
    impl_->force_idr_ = true;
    return impl_->fallback_ ? impl_->fallback_->ForceIdr() : 0;
  });
}

int WmfAv1Encoder::SetTargetBitrate(int bitrate) {
  if (bitrate <= 0) return -1;
  return impl_->worker_.Call([&] {
    bitrate = ClampEncoderTargetBitrate(bitrate, impl_->config_.max_bitrate);
    if (impl_->fallback_) {
      const int result = impl_->fallback_->SetTargetBitrate(bitrate);
      if (result == 0) {
        impl_->config_.init_bitrate = bitrate;
        impl_->config_.average_bitrate = bitrate;
      }
      return result;
    }
    if (!impl_->api_) return -1;
    const HRESULT hr =
        SetUint(impl_->api_.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate);
    if (impl_->peak_constrained_vbr_) {
      SetUint(impl_->api_.Get(), CODECAPI_AVEncCommonMaxBitRate,
              ComputePeakBitrate(bitrate, impl_->config_.max_bitrate));
    }
    if (hr == S_OK) {
      impl_->applied_bitrate_ = bitrate;
    } else if (impl_->applied_bitrate_ <= 0 ||
               std::abs(bitrate - impl_->applied_bitrate_) >=
                   impl_->applied_bitrate_ / 5) {
      impl_->reconfigure_ = true;
    }
    impl_->config_.init_bitrate = bitrate;
    impl_->config_.average_bitrate = bitrate;
    return 0;
  });
}

int WmfAv1Encoder::SetTargetFrameRate(int frame_rate) {
  if (frame_rate <= 0 || frame_rate > 240) return -1;
  return impl_->worker_.Call([&] {
    if (impl_->fallback_) {
      const int result = impl_->fallback_->SetTargetFrameRate(frame_rate);
      if (result == 0) {
        impl_->config_.max_frame_rate = frame_rate;
        impl_->input_frame_rate_ = frame_rate;
      }
      return result;
    }
    if (!impl_->encoder_) return -1;
    if (impl_->config_.max_frame_rate != frame_rate) impl_->reconfigure_ = true;
    impl_->config_.max_frame_rate = frame_rate;
    impl_->input_frame_rate_ = frame_rate;
    return 0;
  });
}

int WmfAv1Encoder::SetPrioritizeEncodingSpeedOverQuality(bool prioritize_speed) {
  return impl_->worker_.Call([&] {
    if (impl_->fallback_ || !impl_->api_) return -1;
    const ULONG target_qvs =
        SelectQualityVsSpeed(impl_->config_, prioritize_speed);
    if (target_qvs == impl_->applied_qvs_) {
      impl_->speed_priority_ = prioritize_speed;
      return 0;
    }
    const HRESULT hr = SetUint(impl_->api_.Get(),
                               CODECAPI_AVEncCommonQualityVsSpeed, target_qvs);
    if (hr == S_OK) {
      impl_->speed_priority_ = prioritize_speed;
      impl_->applied_qvs_ = target_qvs;
      return 0;
    }
    return -1;
  });
}

int WmfAv1Encoder::SetInputFrameRate(int frame_rate) {
  if (frame_rate <= 0 || frame_rate > 240) return -1;
  return impl_->worker_.Call([&] {
    if (!impl_->initialized_) return -1;
    const int fps = std::min(frame_rate, impl_->config_.max_frame_rate);
    if (fps == impl_->input_frame_rate_) return 0;
    if (impl_->fallback_ && impl_->fallback_->SetTargetFrameRate(fps) != 0)
      return -1;
    impl_->input_frame_rate_ = fps;
    return 0;
  });
}

int WmfAv1Encoder::GetResolution(int* width, int* height) const {
  if (!width || !height) return -1;
  return impl_->worker_.Call([&] {
    if (impl_->fallback_) return impl_->fallback_->GetResolution(width, height);
    if (!impl_->encoder_) return -1;
    *width = impl_->config_.init_width;
    *height = impl_->config_.init_height;
    return 0;
  });
}

std::string WmfAv1Encoder::GetEncoderName() const {
  return impl_->worker_.Call([&]() -> std::string {
    return impl_->fallback_ ? impl_->fallback_->GetEncoderName()
                            : "Media Foundation (AV1, HW)";
  });
}
}  // namespace minirtc
