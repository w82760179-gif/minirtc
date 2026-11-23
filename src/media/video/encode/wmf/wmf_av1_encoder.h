/*
 * Windows Media Foundation hardware AV1 encoder (Intel Arc / QSV / NVENC / AMF MFT).
 */

#ifndef _WMF_AV1_ENCODER_H_
#define _WMF_AV1_ENCODER_H_

#include "media_codec.h"

namespace minirtc {

// Uses only hardware AV1 MFTs registered by the graphics driver. No vendor SDK
// is required. Input is packed NV12; output is raw AV1 OBU stream.
class WmfAv1Encoder final : public MediaCodec {
 public:
  explicit WmfAv1Encoder(std::shared_ptr<SystemClock> clock);
  ~WmfAv1Encoder() override;
  static bool IsSupported();
  int Init(const MediaCodecConfig& config) override;
  int Encode(const RawFrame& frame,
             std::function<int(const EncodedFrame&)> callback) override;
  int ForceIdr() override;
  int SetTargetBitrate(int bitrate) override;
  int SetTargetFrameRate(int frame_rate) override;
  bool SupportsInputFrameRateUpdates() const override { return true; }
  int SetInputFrameRate(int frame_rate) override;
  int GetResolution(int* width, int* height) const override;
  std::string GetEncoderName() const override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace minirtc
#endif
