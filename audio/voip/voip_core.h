/*
 *  Copyright (c) 2020 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef AUDIO_VOIP_VOIP_CORE_H_
#define AUDIO_VOIP_VOIP_CORE_H_

#include <map>
#include <memory>
#include <queue>
#include <unordered_map>
#include <vector>

#include "api/audio_codecs/audio_decoder_factory.h"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/scoped_refptr.h"
#include "api/task_queue/task_queue_factory.h"
#include "api/voip/voip_base.h"
#include "api/voip/voip_codec.h"
#include "api/voip/voip_dtmf.h"
#include "api/voip/voip_engine.h"
#include "api/voip/voip_network.h"
#include "api/voip/voip_statistics.h"
#include "audio/audio_transport_impl.h"
#include "audio/voip/audio_channel.h"
#include "modules/audio_device/include/audio_device.h"
#include "modules/audio_mixer/audio_mixer_impl.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "modules/utility/include/process_thread.h"
#include "rtc_base/synchronization/mutex.h"

namespace webrtc {

// VoipCore is the implementatino of VoIP APIs listed in api/voip directory.
// It manages a vector of AudioChannel objects where each is mapped with a
// ChannelId (int) type. ChannelId is the primary key to locate a specific
// AudioChannel object to operate requested VoIP API from the caller.
//
// This class receives required audio components from caller at construction and
// owns the life cycle of them to orchestrate the proper destruction sequence.
class VoipCore : public VoipEngine,
                 public VoipBase,
                 public VoipNetwork,
                 public VoipCodec,
                 public VoipDtmf,
                 public VoipStatistics {
 public:
  // Construct VoipCore with provided arguments.
  // ProcessThread implementation can be injected by |process_thread|
  // (mainly for testing purpose) and when set to nullptr, default
  // implementation will be used.
  VoipCore(rtc::scoped_refptr<AudioEncoderFactory> encoder_factory,
           rtc::scoped_refptr<AudioDecoderFactory> decoder_factory,
           std::unique_ptr<TaskQueueFactory> task_queue_factory,
           rtc::scoped_refptr<AudioDeviceModule> audio_device_module,
           rtc::scoped_refptr<AudioProcessing> audio_processing,
           std::unique_ptr<ProcessThread> process_thread = nullptr);
  ~VoipCore() override = default;

  // Implements VoipEngine interfaces.
  VoipBase& Base() override { return *this; }
  VoipNetwork& Network() override { return *this; }
  VoipCodec& Codec() override { return *this; }
  VoipDtmf& Dtmf() override { return *this; }
  VoipStatistics& Statistics() override { return *this; }

  // Implements VoipBase interfaces.
  absl::optional<ChannelId> CreateChannel(
      Transport* transport,
      absl::optional<uint32_t> local_ssrc) override;
  void ReleaseChannel(ChannelId channel_id) override;
  bool StartSend(ChannelId channel_id) override;
  bool StopSend(ChannelId channel_id) override;
  bool StartPlayout(ChannelId channel_id) override;
  bool StopPlayout(ChannelId channel_id) override;

  // Implements VoipNetwork interfaces.
  void ReceivedRTPPacket(ChannelId channel_id,
                         rtc::ArrayView<const uint8_t> rtp_packet) override;
  void ReceivedRTCPPacket(ChannelId channel_id,
                          rtc::ArrayView<const uint8_t> rtcp_packet) override;

  // Implements VoipCodec interfaces.
  void SetSendCodec(ChannelId channel_id,
                    int payload_type,
                    const SdpAudioFormat& encoder_format) override;
  void SetReceiveCodecs(
      ChannelId channel_id,
      const std::map<int, SdpAudioFormat>& decoder_specs) override;

  // Implements VoipDtmf interfaces.
  void RegisterTelephoneEventType(ChannelId channel_id,
                                  int rtp_payload_type,
                                  int sample_rate_hz) override;
  bool SendDtmfEvent(ChannelId channel_id,
                     DtmfEvent dtmf_event,
                     int duration_ms) override;

  // Implements VoipStatistics interfaces.
  absl::optional<IngressStatistics> GetIngressStatistics(
      ChannelId channel_id) override;

 private:
  // Initialize ADM and default audio device if needed.
  // Returns true if ADM is successfully initialized or already in such state
  // (e.g called more than once). Returns false when ADM fails to initialize
  // which would presumably render further processing useless. Note that such
  // failure won't necessarily succeed in next initialization attempt as it
  // would mean changing the ADM implementation. From Android N and onwards, the
  // mobile app may not be able to gain microphone access when in background
  // mode. Therefore it would be better to delay the logic as late as possible.
  bool InitializeIfNeeded();

  // Fetches the corresponding AudioChannel assigned with given |channel|.
  // Returns nullptr if not found.
  rtc::scoped_refptr<AudioChannel> GetChannel(ChannelId channel_id);

  // Updates AudioTransportImpl with a new set of actively sending AudioSender
  // (AudioEgress). This needs to be invoked whenever StartSend/StopSend is
  // involved by caller. Returns false when the selected audio device fails to
  // initialize where it can't expect to deliver any audio input sample.
  bool UpdateAudioTransportWithSenders();

  // Synchronization for these are handled internally.
  rtc::scoped_refptr<AudioEncoderFactory> encoder_factory_;
  rtc::scoped_refptr<AudioDecoderFactory> decoder_factory_;
  std::unique_ptr<TaskQueueFactory> task_queue_factory_;

  // Synchronization is handled internally by AudioProcessing.
  // Must be placed before |audio_device_module_| for proper destruction.
  rtc::scoped_refptr<AudioProcessing> audio_processing_;

  // Synchronization is handled internally by AudioMixer.
  // Must be placed before |audio_device_module_| for proper destruction.
  rtc::scoped_refptr<AudioMixer> audio_mixer_;

  // Synchronization is handled internally by AudioTransportImpl.
  // Must be placed before |audio_device_module_| for proper destruction.
  std::unique_ptr<AudioTransportImpl> audio_transport_;

  // Synchronization is handled internally by AudioDeviceModule.
  rtc::scoped_refptr<AudioDeviceModule> audio_device_module_;

  // Synchronization is handled internally by ProcessThread.
  // Must be placed before |channels_| for proper destruction.
  std::unique_ptr<ProcessThread> process_thread_;

  Mutex lock_;

  // Member to track a next ChannelId for new AudioChannel.
  int next_channel_id_ RTC_GUARDED_BY(lock_) = 0;

  // Container to track currently active AudioChannel objects mapped by
  // ChannelId.
  std::unordered_map<ChannelId, rtc::scoped_refptr<AudioChannel>> channels_
      RTC_GUARDED_BY(lock_);

  // Boolean flag to ensure initialization only occurs once.
  bool initialized_ RTC_GUARDED_BY(lock_) = false;
};

}  // namespace webrtc

#endif  // AUDIO_VOIP_VOIP_CORE_H_