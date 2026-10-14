#pragma once

#include <stdint.h>     // for uint32_t, uint8_t
#include <functional>   // for function
#include <memory>       // for shared_ptr, unique_ptr
#include <string>       // for string
#include <string_view>  // for string_view
#include <variant>      // for variant
#include <vector>       // for vector

#include "CDNAudioFile.h"  // for CDNTrackStream, CDNTrackStream::Track...
#include "TrackQueue.h"
#include "protobuf/spirc.pb.h"  // for MessageType

namespace cspot {
class TrackPlayer;
struct Context;

class SpircHandler {
 public:
  SpircHandler(std::shared_ptr<cspot::Context> ctx);

  enum class EventType {
    PLAY_PAUSE,
    VOLUME,
    TRACK_INFO,
    DISC,
    NEXT,
    PREV,
    SEEK,
    DEPLETED,
    FLUSH,
    PLAYBACK_START
  };

  typedef std::variant<TrackInfo, int, bool> EventData;

  struct Event {
    EventType eventType;
    EventData data;
  };

  typedef std::function<void(std::unique_ptr<Event>)> EventHandler;

  void subscribeToMercury();
  std::shared_ptr<TrackPlayer> getTrackPlayer();

  void setEventHandler(EventHandler handler);

  void setPause(bool pause);

  bool previousSong();

  bool nextSong();

  void notifyAudioReachedPlayback(std::string_view identifier = {});
  void notifyAudioEnded();
  void updatePositionMs(uint32_t position);
  void setRemoteVolume(int volume);
  void loadTrackFromURI(const std::string& uri);

  /**
   * @brief Starts playing a context chosen on this device (not sent by a phone).
   *
   * Does what a Load frame does, with a track list resolved by the caller
   * (for example from spclient's context-resolve), then notifies so other
   * devices see this one playing it.
   *
   * @param contextUri the playlist/album/... the tracks come from
   * @param trackUris spotify:track:/spotify:episode: URIs, in play order
   * @param index the track to start with
   * @param shuffled tells other devices the order is shuffled (the caller
   *        shuffles trackUris; cspot does not shuffle by itself)
   * @return false if there is nothing to play at index
   */
  bool loadContext(const std::string& contextUri,
                   const std::vector<std::string>& trackUris, uint32_t index,
                   bool shuffled = false);

  /**
   * @brief Adds tracks to the end of a context started with loadContext(),
   * without interrupting playback, then notifies.
   */
  void appendToContext(const std::vector<std::string>& trackUris);

  // The context playing now (set by loadContext or a phone's Load frame).
  std::string contextUri();

  // Seeks the playing track, as a phone's Seek frame does.
  void seekMs(uint32_t position);
  // Plays the track at `index` of the current list, as if picked in a queue.
  bool jumpTo(uint32_t index);

  std::shared_ptr<cspot::TrackQueue> getTrackQueue() { return trackQueue; }

  void disconnect();

 private:
  std::shared_ptr<cspot::Context> ctx;
  std::shared_ptr<cspot::TrackPlayer> trackPlayer;
  std::shared_ptr<cspot::TrackQueue> trackQueue;

  EventHandler eventHandler = nullptr;

  std::shared_ptr<cspot::PlaybackState> playbackState;

  void sendCmd(MessageType typ);

  void sendEvent(EventType type);
  void sendEvent(EventType type, EventData data);

  bool skipSong(TrackQueue::SkipDirection dir);
  void handleFrame(std::vector<uint8_t>& data);
  void notify();
};
}  // namespace cspot
