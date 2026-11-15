#include "sendspin_media_source.h"

#if defined(USE_ESP_IDF) && defined(USE_SENDSPIN_CONTROLLER) && defined(USE_SENDSPIN_PLAYER)

#include "esphome/components/audio/audio.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cmath>

namespace esphome::sendspin_ {

static const char *const TAG = "sendspin.media_source";

static constexpr char URI_PREFIX[] = "sendspin://";

namespace {

const char *media_source_state_to_string(media_source::MediaSourceState state) {
  switch (state) {
    case media_source::MediaSourceState::IDLE:
      return "idle";
    case media_source::MediaSourceState::PLAYING:
      return "playing";
    case media_source::MediaSourceState::PAUSED:
      return "paused";
    case media_source::MediaSourceState::ERROR:
      return "error";
  }
  return "unknown";
}

}  // namespace

void SendspinMediaSource::setup() {
  this->player_role_ = this->parent_->get_player_role();
  if (!this->player_role_) {
    ESP_LOGE(TAG, "Failed to get player role from hub");
    this->mark_failed();
    return;
  }

  // Push cached states to player role. They may have been set before setup() ran.
  this->player_role_->update_volume(std::roundf(this->cached_volume_ * 100.0f));
  this->player_role_->update_muted(this->cached_muted_);
  this->player_role_->set_output_delay_adjustable(this->static_delay_adjustable_);
}

void SendspinMediaSource::dump_config() {
  ESP_LOGCONFIG(TAG, "Sendspin Media Source: static_delay_adjustable=%s", YESNO(this->static_delay_adjustable_));
}

// THREAD CONTEXT: Main loop (invoked from ESPHome actions / config)
void SendspinMediaSource::set_static_delay_adjustable(bool adjustable) {
  this->static_delay_adjustable_ = adjustable;
  if (this->player_role_) {
    this->player_role_->set_output_delay_adjustable(adjustable);
  }
}

// --- MediaSource interface ---

bool SendspinMediaSource::can_handle(const std::string &uri) const { return uri.starts_with(URI_PREFIX); }

// THREAD CONTEXT: Main loop (media_source.h documents play_uri as main-loop only)
bool SendspinMediaSource::play_uri(const std::string &uri) {
  // The queued request has been delivered, whatever the outcome, so the next stream start may request again
  this->pending_start_ = false;
  if (!this->is_ready() || this->is_failed() || !this->has_listener()) {
    return false;
  }

  if (this->get_state() != media_source::MediaSourceState::IDLE) {
    ESP_LOGE(TAG, "Cannot play '%s': source is busy", uri.c_str());
    return false;
  }

  if (!this->parent_->is_client_running()) {
    ESP_LOGE(TAG, "Cannot play '%s': Sendspin is disabled", uri.c_str());
    return false;
  }

  if (!uri.starts_with(URI_PREFIX)) {
    ESP_LOGE(TAG, "Invalid URI: '%s'", uri.c_str());
    return false;
  }

  std::string sendspin_id = uri.substr(sizeof(URI_PREFIX) - 1);

  if (sendspin_id.empty()) {
    ESP_LOGE(TAG, "Invalid URI: '%s'", uri.c_str());
    return false;
  }

  ESP_LOGD(TAG, "sendspin_id: %s", sendspin_id.c_str());

  if (sendspin_id != "current") {
    // Connect to a new server as a websocket client
    this->parent_->connect_to_server("ws://" + sendspin_id);
  }

  // Tell the orchestrator we're now playing so it routes audio output from us
  this->set_state_(media_source::MediaSourceState::PLAYING);

  return true;
}

// THREAD CONTEXT: Main loop (media_source.h documents handle_command as main-loop only)
void SendspinMediaSource::handle_command(media_source::MediaSourceCommand command) {
  if (!this->parent_->is_client_running()) {
    if (command == media_source::MediaSourceCommand::STOP) {
      // Nothing is playing, so the orchestrator gets its pipeline back straight away
      this->on_stream_end();
    } else {
      ESP_LOGW(TAG, "Cannot handle command: Sendspin is disabled");
    }
    return;
  }
  switch (command) {
    case media_source::MediaSourceCommand::STOP: {
      if (!this->pending_start_) {
        // Ignore stop commands if we have a pending start, since the orchestrator may send a stop command before
        // play_uri
        ESP_LOGD(TAG, "Received STOP command, leaving the Sendspin group");
        this->parent_->leave_group();
      }
      break;
    }
    case media_source::MediaSourceCommand::PLAY:  // NOLINT(bugprone-branch-clone)
      this->parent_->send_client_command(sendspin::SendspinControllerCommand::PLAY, std::nullopt, std::nullopt);
      break;
    case media_source::MediaSourceCommand::PAUSE:
      this->parent_->send_client_command(sendspin::SendspinControllerCommand::PAUSE, std::nullopt, std::nullopt);
      break;
    case media_source::MediaSourceCommand::NEXT:
      this->parent_->send_client_command(sendspin::SendspinControllerCommand::NEXT, std::nullopt, std::nullopt);
      break;
    case media_source::MediaSourceCommand::PREVIOUS:
      this->parent_->send_client_command(sendspin::SendspinControllerCommand::PREVIOUS, std::nullopt, std::nullopt);
      break;
    case media_source::MediaSourceCommand::REPEAT_ALL:
      this->parent_->send_client_command(sendspin::SendspinControllerCommand::REPEAT_ALL, std::nullopt, std::nullopt);
      break;
    case media_source::MediaSourceCommand::REPEAT_ONE:
      this->parent_->send_client_command(sendspin::SendspinControllerCommand::REPEAT_ONE, std::nullopt, std::nullopt);
      break;
    case media_source::MediaSourceCommand::REPEAT_OFF:
      this->parent_->send_client_command(sendspin::SendspinControllerCommand::REPEAT_OFF, std::nullopt, std::nullopt);
      break;
    case media_source::MediaSourceCommand::SHUFFLE:
      this->parent_->send_client_command(sendspin::SendspinControllerCommand::SHUFFLE, std::nullopt, std::nullopt);
      break;
    case media_source::MediaSourceCommand::UNSHUFFLE:
      this->parent_->send_client_command(sendspin::SendspinControllerCommand::UNSHUFFLE, std::nullopt, std::nullopt);
      break;
    default:
      break;
  }
}

// THREAD CONTEXT: Main loop (orchestrator -> source notification)
void SendspinMediaSource::notify_volume_changed(float volume) {
  this->cached_volume_ = volume;
  if (this->player_role_) {
    this->player_role_->update_volume(std::roundf(volume * 100.0f));
  }
}

// THREAD CONTEXT: Main loop (orchestrator -> source notification)
void SendspinMediaSource::notify_mute_changed(bool is_muted) {
  this->cached_muted_ = is_muted;
  if (this->player_role_) {
    this->player_role_->update_muted(is_muted);
  }
}

// THREAD CONTEXT: Speaker playback callback thread (forwarded from the speaker).
// PlayerRole::notify_audio_played() is documented as thread-safe for this use.
void SendspinMediaSource::notify_audio_played(uint32_t frames, int64_t timestamp) {
  if (this->player_role_) {
    this->player_role_->notify_audio_played(frames, timestamp);
  }
}

bool SendspinMediaSource::connected() const { return this->parent_ != nullptr && this->parent_->is_server_connected(); }

uint32_t SendspinMediaSource::buffered_ms() const {
  const auto bytes = this->last_buffered_bytes_.load(std::memory_order_relaxed);
  const auto rate = this->last_sample_rate_.load(std::memory_order_relaxed);
  const auto bytes_per_frame = this->last_bytes_per_frame_.load(std::memory_order_relaxed);
  if (rate == 0 || bytes_per_frame == 0) {
    return 0;
  }
  return static_cast<uint32_t>((static_cast<uint64_t>(bytes) * 1000ULL) /
                               (static_cast<uint64_t>(rate) * bytes_per_frame));
}

uint32_t SendspinMediaSource::buffered_percent() const {
  const auto bytes = this->last_buffered_bytes_.load(std::memory_order_relaxed);
  const auto max_bytes = this->max_buffered_bytes_.load(std::memory_order_relaxed);
  if (max_bytes == 0) {
    return 0;
  }
  return static_cast<uint32_t>((static_cast<uint64_t>(bytes) * 100ULL) / max_bytes);
}

std::string SendspinMediaSource::debug_summary() const {
  const auto calls = this->write_call_count_.load(std::memory_order_relaxed);
  const auto short_writes = this->short_write_count_.load(std::memory_order_relaxed);
  const auto zero_writes = this->zero_write_count_.load(std::memory_order_relaxed);
  const auto stalled_writes = this->stalled_write_count_.load(std::memory_order_relaxed);
  const auto last_write_ms = this->last_write_ms_.load(std::memory_order_relaxed);
  const auto longest_write_ms = this->longest_write_ms_.load(std::memory_order_relaxed);
  const auto last_timeout_ms = this->last_write_timeout_ms_.load(std::memory_order_relaxed);
  const auto sample_rate = this->last_sample_rate_.load(std::memory_order_relaxed);
  const auto channels = this->last_channels_.load(std::memory_order_relaxed);
  const auto in_bytes = this->input_bytes_.load(std::memory_order_relaxed);
  const auto out_bytes = this->output_bytes_.load(std::memory_order_relaxed);
  const auto recovery_restarts = this->parent_ != nullptr ? this->parent_->disconnect_recovery_restart_count() : 0;

  return str_sprintf(
      "conn=%s state=%s writes=%lu short=%lu zero=%lu stalls=%lu last=%lums longest=%lums timeout=%lums rate=%luHz ch=%lu in=%lu out=%lu restart=%lu hub={%s}",
      YESNO(this->connected()), media_source_state_to_string(this->get_state()), static_cast<unsigned long>(calls),
      static_cast<unsigned long>(short_writes), static_cast<unsigned long>(zero_writes),
      static_cast<unsigned long>(stalled_writes), static_cast<unsigned long>(last_write_ms),
      static_cast<unsigned long>(longest_write_ms), static_cast<unsigned long>(last_timeout_ms),
      static_cast<unsigned long>(sample_rate), static_cast<unsigned long>(channels),
      static_cast<unsigned long>(in_bytes), static_cast<unsigned long>(out_bytes),
      static_cast<unsigned long>(recovery_restarts),
      this->parent_ != nullptr ? this->parent_->debug_summary().c_str() : "hub=none");
}

void SendspinMediaSource::dump_debug() const { ESP_LOGI(TAG, "Debug snapshot: %s", this->debug_summary().c_str()); }

// --- Sendspin PlayerRoleListener overrides ---

// THREAD CONTEXT: Sendspin sync task background thread. May block up to timeout_ms.
size_t SendspinMediaSource::on_audio_write(uint8_t *data, size_t length, uint32_t timeout_ms) {
  this->write_call_count_.fetch_add(1, std::memory_order_relaxed);
  this->input_bytes_.fetch_add(length, std::memory_order_relaxed);
  this->last_write_timeout_ms_.store(timeout_ms, std::memory_order_relaxed);

  if (!this->has_listener() || (this->get_state() != media_source::MediaSourceState::PLAYING)) {
    vTaskDelay(pdMS_TO_TICKS(timeout_ms));
    this->zero_write_count_.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }

  // PlayerRole::get_current_stream_params() is safe to call from the sync task.
  auto &params = this->player_role_->get_current_stream_params();
  if (!params.bit_depth.has_value() || !params.channels.has_value() || !params.sample_rate.has_value()) {
    vTaskDelay(pdMS_TO_TICKS(timeout_ms));
    this->zero_write_count_.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  this->last_sample_rate_.store(*params.sample_rate, std::memory_order_relaxed);
  this->last_channels_.store(*params.channels, std::memory_order_relaxed);
  audio::AudioStreamInfo stream_info(*params.bit_depth, *params.channels, *params.sample_rate);
  this->last_bytes_per_frame_.store(
      static_cast<uint32_t>((*params.bit_depth * *params.channels) / 8), std::memory_order_relaxed);
  this->last_buffered_bytes_.store(static_cast<uint32_t>(length), std::memory_order_relaxed);
  uint32_t max_bytes = this->max_buffered_bytes_.load(std::memory_order_relaxed);
  while (static_cast<uint32_t>(length) > max_bytes &&
         !this->max_buffered_bytes_.compare_exchange_weak(max_bytes, static_cast<uint32_t>(length),
                                                          std::memory_order_relaxed)) {
  }

  const uint32_t started_ms = millis();
  size_t written = this->write_output(data, length, timeout_ms, stream_info);
  uint32_t elapsed_ms = millis() - started_ms;
  this->last_write_ms_.store(elapsed_ms, std::memory_order_relaxed);
  this->output_bytes_.fetch_add(written, std::memory_order_relaxed);

  uint32_t longest = this->longest_write_ms_.load(std::memory_order_relaxed);
  while (elapsed_ms > longest &&
         !this->longest_write_ms_.compare_exchange_weak(longest, elapsed_ms, std::memory_order_relaxed)) {
  }

  if (elapsed_ms >= 500) {
    this->stalled_write_count_.fetch_add(1, std::memory_order_relaxed);
  }
  if (written == 0) {
    this->zero_write_count_.fetch_add(1, std::memory_order_relaxed);
  } else if (written < length) {
    this->short_write_count_.fetch_add(1, std::memory_order_relaxed);
  }

  return written;
}

// THREAD CONTEXT: Main loop (PlayerRoleListener lifecycle callback)
void SendspinMediaSource::on_stream_start() {
  if (!this->pending_start_) {
    // Dedup rapid on_stream_start() calls
    this->pending_start_ = true;
    // Request the orchestrator to start this source
    this->request_play_uri_("sendspin://current");
  }
}

// THREAD CONTEXT: Main loop (PlayerRoleListener lifecycle callback)
void SendspinMediaSource::on_stream_end() {
  if (this->get_state() != media_source::MediaSourceState::IDLE) {
    // Only set to IDLE if we were previously in a non-IDLE state, to avoid duplicate state changes
    this->set_state_(media_source::MediaSourceState::IDLE);
  }
}

// THREAD CONTEXT: Main loop (PlayerRoleListener callback)
void SendspinMediaSource::on_volume_changed(uint8_t volume) { this->request_volume_(volume / 100.0f); }

// THREAD CONTEXT: Main loop (PlayerRoleListener callback)
void SendspinMediaSource::on_mute_changed(bool muted) { this->request_mute_(muted); }

}  // namespace esphome::sendspin_

#endif  // USE_ESP_IDF && USE_SENDSPIN_PLAYER && USE_SENDSPIN_CONTROLLER
