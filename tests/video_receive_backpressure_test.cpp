#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "rtp_packetizer_av1.h"
#include "rtp_packetizer_h264.h"
#include "rtp_video_receiver.h"

namespace {

constexpr uint32_t kMediaSsrc = 42;
constexpr uint32_t kRtxSsrc = 43;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

class Fixture {
 public:
  explicit Fixture(bool av1 = false)
      : clock_(std::make_shared<minirtc::SystemClock>(
            [this] { return now_ms_.load() * 1'000'000; },
            [] { return 1'700'000'000'000'000'000LL; })),
        av1_(av1),
        receiver_(clock_) {
    using minirtc::rtp::PAYLOAD_TYPE;
    receiver_.SetMediaConfig(kMediaSsrc, kRtxSsrc,
                             av1_ ? PAYLOAD_TYPE::AV1 : PAYLOAD_TYPE::H264);
    receiver_.SetFrameAdmissionFunc([this](bool) { return decoder_ready; });
    receiver_.SetOnReceiveCompleteFrame(
        [this](std::unique_ptr<minirtc::ReceivedFrame> frame) {
          Check(frame->Size() > 0, "empty frame delivered");
          received_.push_back(frame->Buffer()[frame->Size() - 1]);
        });
    receiver_.SetSendDataFunc([this](const char* data, size_t size) {
      // A standalone RTCP payload-specific feedback packet with FIR format.
      if (size >= 4 && (static_cast<uint8_t>(data[0]) & 31) == 4 &&
          static_cast<uint8_t>(data[1]) == 206) {
        ++fir_requests;
      }
      return 0;
    });
  }

  std::unique_ptr<minirtc::RtpPacket> MakeFrame(
      uint8_t id, bool keyframe = false) {
    uint8_t payload[] = {0, 0, 0, 1, uint8_t(keyframe ? 0x65 : 0x41), id};
    uint8_t obu[] = {0x32, 0x01, id};  // Frame OBU with a one-byte payload.
    av1_packetizer_.SetIsKeyFrame(keyframe);
    auto packets = av1_ ? av1_packetizer_.Build(obu, sizeof(obu), id * 1'000, false)
                       : h264_packetizer_.Build(payload, sizeof(payload),
                                                id * 1'000, false);
    Check(packets.size() == 1, "expected one RTP packet");
    return std::move(packets.front());
  }

  void Send(uint8_t id, bool keyframe = false) {
    auto packet = MakeFrame(id, keyframe);
    Insert(*packet);
  }

  void InsertFirstH264Fragment(uint8_t id) {
    std::vector<uint8_t> payload(2000, id);
    auto packets = h264_packetizer_.Build(payload.data(), payload.size(),
                                          id * 1'000, false);
    Check(packets.size() > 1, "expected a fragmented H264 frame");
    Insert(*packets.front());
  }

  void Insert(minirtc::RtpPacket& packet) { receiver_.InsertRtpPacket(packet); }

  void InsertRtx(const minirtc::RtpPacket& packet) {
    const auto original = packet.Buffer();
    std::vector<uint8_t> bytes(original.data(), original.data() + original.size());
    Check(bytes.size() > 12 && bytes[0] == 0x80, "unexpected RTP header");
    const uint8_t old_sequence[] = {bytes[2], bytes[3]};
    bytes.insert(bytes.begin() + 12, old_sequence, old_sequence + 2);
    bytes[1] = (bytes[1] & 0x80) | minirtc::rtp::PAYLOAD_TYPE::RTX;
    bytes[2] = 0;
    bytes[3] = 1;
    bytes[11] = kRtxSsrc;
    minirtc::RtpPacket rtx;
    Check(rtx.Build(bytes.data(), bytes.size()), "invalid RTX packet");
    Insert(rtx);
  }

  void Process() { static_cast<minirtc::ThreadBase&>(receiver_).Process(); }
  void Advance(int64_t ms) { now_ms_ += ms; }

  void Expect(std::initializer_list<uint8_t> frames, const char* message) {
    Check(received_ == std::vector<uint8_t>(frames), message);
  }

  bool decoder_ready = false;
  std::atomic<int> fir_requests{0};

 private:
  std::atomic<int64_t> now_ms_{1000};
  std::shared_ptr<minirtc::SystemClock> clock_;
  std::vector<uint8_t> received_;
  bool av1_;
  minirtc::RtpPacketizerH264 h264_packetizer_{kMediaSsrc};
  minirtc::RtpPacketizerAv1 av1_packetizer_{kMediaSsrc};
  minirtc::RtpVideoReceiver receiver_;
};

void BriefBackpressurePreservesOrder() {
  Fixture test;
  test.Send(1);
  test.Send(2);
  test.Process();
  test.Expect({}, "decoder should still be full");
  test.Advance(99);
  test.decoder_ready = true;
  test.Process();
  test.Expect({1, 2}, "brief backpressure changed delivery order");
  Check(test.fir_requests == 0, "brief backpressure requested a keyframe");
}

void ExpiredDeltaResumesAtQueuedKeyframe(bool av1) {
  Fixture test(av1);
  auto stale = test.MakeFrame(1);
  test.Insert(*stale);
  test.Process();
  test.Advance(50);
  test.Insert(*stale);
  test.InsertRtx(*stale);
  test.Send(2);
  test.Send(3, true);
  test.Send(4);
  test.Advance(50);
  test.decoder_ready = true;
  test.Process();
  test.Expect({3, 4}, "expired delta or its keyframe was handled incorrectly");
  Check(test.fir_requests == 0, "queued keyframe should suppress FIR");
  test.Insert(*stale);
  test.InsertRtx(*stale);
  test.Process();
  test.Expect({3, 4}, "late packet resurrected a discarded frame");
}

void ExpiredDeltaRequestsOneKeyframe() {
  Fixture test;
  test.Send(1);
  test.Send(2);
  test.Process();
  test.Advance(100);
  test.Process();
  test.Send(3);
  test.decoder_ready = true;
  test.Process();
  test.Expect({}, "delta was delivered without a reference chain");
  Check(test.fir_requests == 1, "expected one rate-limited keyframe request");
  test.Send(4, true);
  test.Send(5);
  test.Process();
  test.Expect({4, 5}, "did not resume from the requested keyframe");
}

void IdleDesktopKeepsLastUpdate() {
  Fixture test;
  test.Send(1);
  test.Process();
  test.Advance(150);
  test.decoder_ready = true;
  test.Process();
  test.Expect({1}, "last update of an idle desktop was discarded");
  Check(test.fir_requests == 0, "idle desktop requested an unnecessary keyframe");
}

void IncompleteSuccessorDoesNotDiscardLastUpdate() {
  Fixture test;
  test.Send(1);
  test.Process();
  test.InsertFirstH264Fragment(2);
  test.Advance(150);
  test.decoder_ready = true;
  test.Process();
  test.Expect({1}, "incomplete successor caused the last update to be discarded");
  Check(test.fir_requests == 0, "the fragment should keep its repair window");
}

void PacketRepairHasItsOwnDeadline() {
  Fixture test;
  test.decoder_ready = true;
  test.Send(1, true);
  test.Process();
  auto missing = test.MakeFrame(2);
  test.Send(3);
  test.Process();
  test.Expect({1}, "packet gap did not hold the newer delta");
  test.Advance(120);
  test.InsertRtx(*missing);
  test.Process();
  test.Expect({1, 2, 3}, "decode admission shortened the packet repair window");
  Check(test.fir_requests == 0, "successful RTX should preserve the chain");
}

void StalledDecoderRetainsKeyframeForRecovery() {
  Fixture test;
  test.Send(1, true);
  test.Send(2);
  test.Process();
  test.Advance(1500);
  test.Process();
  test.decoder_ready = true;
  test.Process();
  test.Expect({1, 2}, "decode admission discarded a complete recovery keyframe");
  Check(test.fir_requests == 0, "held keyframe should suppress FIR");
}

}  // namespace

int main() {
  try {
    BriefBackpressurePreservesOrder();
    ExpiredDeltaResumesAtQueuedKeyframe(false);
    ExpiredDeltaResumesAtQueuedKeyframe(true);
    ExpiredDeltaRequestsOneKeyframe();
    IdleDesktopKeepsLastUpdate();
    IncompleteSuccessorDoesNotDiscardLastUpdate();
    PacketRepairHasItsOwnDeadline();
    StalledDecoderRetainsKeyframeForRecovery();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "PASS: 8 video receiver admission and recovery cases\n";
  return 0;
}
