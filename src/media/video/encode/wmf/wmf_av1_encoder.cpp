#include "wmf_av1_encoder.h"

#include <codecapi.h>
#include <strmif.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
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
  const int64_t burst = static_cast<int64_t>(target_bps) * 5 / 4;
  const int64_t cap =
      max_bps > 0 ? std::max<int64_t>(target_bps, max_bps) : burst;
  const int64_t clamped = std::min<int64_t>(
      std::min<int64_t>(burst, cap), std::numeric_limits<int>::max());
  return static_cast<int>(std::max<int64_t>(target_bps, clamped));
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
    SetUint(api_.Get(), CODECAPI_AVEncCommonQualityVsSpeed,
            config_.video_degradation_preference ==
                    MiniRtcVideoDegradationPreference::MaintainFrameRate
                ? 0
                : 50);

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
              "Media Foundation hardware AV1 encoder: {} (rc={}, {}x{} @ {}fps, "
              "target={} bps, max={} bps)",
              utf8, peak_constrained_vbr_ ? "PeakConstrainedVBR" : "CBR",
              config_.init_width, config_.init_height, config_.max_frame_rate,
              config_.init_bitrate, config_.max_bitrate);
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
      bool have_output = !async_;
      bool received_event = false;
      if (async_) {
        ComPtr<IMFMediaEvent> event;
        hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (hr != MF_E_NO_EVENTS_AVAILABLE) {
          if (FAILED(hr)) return hr;
          received_event = true;
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
      }
      if (!submitted && (!async_ || input_credit_)) {
        hr = encoder_->ProcessInput(input_id_, input.Get(), 0);
        if (SUCCEEDED(hr)) {
          submitted = true;
          if (async_) --input_credit_;
        } else if (hr != MF_E_NOTACCEPTING) {
          return hr;
        }
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
      if (!received_event)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
  }

  wmf::CodecThread worker_;
  std::shared_ptr<SystemClock> clock_;
  MediaCodecConfig config_;
  ComPtr<IMFTransform> encoder_;
  ComPtr<IMFMediaEventGenerator> events_;
  ComPtr<ICodecAPI> api_;
  DWORD input_id_ = 0, output_id_ = 0;
  unsigned input_credit_ = 0;
  bool async_ = false;
  bool peak_constrained_vbr_ = false;
  bool force_idr_ = true;
  bool reconfigure_ = false;
  bool initialized_ = false;
  int applied_bitrate_ = 0;
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
