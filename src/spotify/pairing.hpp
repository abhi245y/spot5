// ps5-spotify - Zeroconf pairing: the phone sends its login to the console.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// While a Pairing exists, a helper payload (helper/mdns_helper.c; apps may not
// use the mDNS port, and a media app may not listen at all) advertises the
// console on the local network (mDNS, _spotify-connect._tcp) and answers the
// phone:
//   GET  /spotify_info   who we are, and our half of the key exchange
//   POST /spotify_info   the phone's encrypted login, passed to us
// over a loopback link we open to it. take_login() reads that link without
// blocking and decrypts on the caller's thread.
//
// ponytail: only used while nothing is saved; a second account (multi-account)
// would keep this running next to a live session.

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>

namespace cspot
{
class LoginBlob;
} // namespace cspot

namespace spotify
{

class Pairing
{
  public:
    // Starts advertising as `device_name`. Returns nullptr and sets `error`
    // (a message for the user) if the helper cannot start.
    static std::unique_ptr<Pairing> start(const std::string &device_name, std::string &error);

    // Stops the helper (advertising and the web server).
    ~Pairing();
    Pairing(const Pairing &) = delete;
    Pairing &operator=(const Pairing &) = delete;

    // The phone's login once it has arrived and decrypted, else nullptr.
    // Sets `error` if the phone sent something that could not be read.
    std::shared_ptr<cspot::LoginBlob> take_login(std::string &error);
    // The helper went away; pairing needs starting again.
    bool lost() const
    {
        return lost_;
    }

  private:
    Pairing() = default;

    std::optional<std::map<std::string, std::string>> next_query();

    std::shared_ptr<cspot::LoginBlob> blob_;
    std::string info_json_; // GET reply, built once on the caller's thread
    int control_ = -1;      // the link to the helper (non-blocking)
    std::string inbox_;     // what the helper sent, not yet whole
    bool lost_ = false;
};

} // namespace spotify
