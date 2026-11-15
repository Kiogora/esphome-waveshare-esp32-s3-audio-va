#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP_IDF) && defined(USE_SENDSPIN_CONTROLLER) && defined(USE_SENDSPIN_PLAYER)

#include "esphome/components/sendspin/sendspin_hub.h"

#include "esphome/components/media_source/media_source.h"

#include <atomic>
#include <string>

#include <sendspin/player_role.h>

namespace esphome::sendspin_ {

/// @brief Thin adapter media source for Sendspin.
///
/// Implements PlayerRoleListener to receive audio data from the sendspin-cpp library's
/// SyncTask and bridges it to ESPHome's MediaSource output pipeline. Also forwards
/// transport commands to the hub's controller role.
class SendspinMediaSource final : public SendspinChild,
                                  public media_source::MediaSource,
                                  public sendspin::PlayerRoleListener {
 public:
  void setup() override;
  void dump_config() override;

  void set_static_delay_adjustable(bool adjustable);

  // MediaSource interface implementation
  bool play_uri(const std::string &uri) override;
  void handle_command(media_source::MediaSourceCommand command) override;
  bool can_handle(const std::string &uri) const override;
  bool has_internal_playlist() const override { return true; }

  void notify_volume_changed(float volume) override;
  void notify_mute_changed(bool is_muted) override;
  void notify_audio_played(uint32_t frames, int64_t timestamp) override;

  bool connected() const;
  uint32_t write_call_count() const { return this->write_call_count_.load(std::memory_order_relaxed); }
  uint32_t short_write_count() const { return this->short_write_count_.load(std::memory_order_relaxed); }
  uint32_t zero_write_count() const { return this->zero_write_count_.load(std::memory_order_relaxed); }
  uint32_t stalled_write_count() const { return this->stalled_write_count_.load(std::memory_order_relaxed); }
  uint32_t last_write_ms() const { return this->last_write_ms_.load(std::memory_order_relaxed); }
  uint32_t longest_write_ms() const { return this->longest_write_ms_.load(std::memory_order_relaxed); }
  uint32_t last_write_timeout_ms() const { return this->last_write_timeout_ms_.load(std::memory_order_relaxed); }
  uint32_t last_sample_rate() const { return this->last_sample_rate_.load(std::memory_order_relaxed); }
  uint32_t last_channels() const { return this->last_channels_.load(std::memory_order_relaxed); }
  size_t input_bytes() const { return this->input_bytes_.load(std::memory_order_relaxed); }
  size_t output_bytes() const { return this->output_bytes_.load(std::memory_order_relaxed); }
  uint32_t last_buffered_bytes() const { return this->last_buffered_bytes_.load(std::memory_order_relaxed); }
  uint32_t max_buffered_bytes() const { return this->max_buffered_bytes_.load(std::memory_order_relaxed); }
  uint32_t buffered_ms() const;
  uint32_t buffered_percent() const;
  std::string debug_summary() const;
  void dump_debug() const;

 protected:
  // --- Sendspin PlayerRoleListener overrides ---

  /// @brief Writes decoded PCM audio to ESPHome's media source output pipeline.
  /// Called from the sync task's background thread.
  size_t on_audio_write(uint8_t *data, size_t length, uint32_t timeout_ms) override;

  /// @brief Called when a new audio stream starts (main loop thread).
  void on_stream_start() override;

  /// @brief Called when the audio stream ends (main loop thread).
  void on_stream_end() override;

  /// @brief Called when volume changes (main loop thread).
  void on_volume_changed(uint8_t volume) override;

  /// @brief Called when mute state changes (main loop thread).
  void on_mute_changed(bool muted) override;

  sendspin::PlayerRole *player_role_{nullptr};

  float cached_volume_{0.0f};

  bool cached_muted_{false};
  bool pending_start_{false};
  bool static_delay_adjustable_{false};

  std::atomic<uint32_t> write_call_count_{0};
  std::atomic<uint32_t> short_write_count_{0};
  std::atomic<uint32_t> zero_write_count_{0};
  std::atomic<uint32_t> stalled_write_count_{0};
  std::atomic<uint32_t> last_write_ms_{0};
  std::atomic<uint32_t> longest_write_ms_{0};
  std::atomic<uint32_t> last_write_timeout_ms_{0};
  std::atomic<uint32_t> last_sample_rate_{0};
  std::atomic<uint32_t> last_channels_{0};
  std::atomic<size_t> input_bytes_{0};
  std::atomic<size_t> output_bytes_{0};
  std::atomic<uint32_t> last_buffered_bytes_{0};
  std::atomic<uint32_t> max_buffered_bytes_{0};
  std::atomic<uint32_t> last_bytes_per_frame_{0};
};

}  // namespace esphome::sendspin_

#endif
