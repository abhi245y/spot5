// ps5-spotify - Zeroconf pairing: the phone sends its login to the console.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// While a Pairing exists, the console is advertised on the local network
// (mDNS, _spotify-connect._tcp, by a helper payload: apps may not use the
// mDNS port) and a small web server answers the phone:
//   GET  /spotify_info   who we are, and our half of the key exchange
//   POST /spotify_info   the phone's encrypted login
// The web server's threads only copy strings; decrypting the login happens
// on the caller's thread in take_login(), because those threads run on the
// system's default (small) stack.
//
// ponytail: only used while nothing is saved; a second account (multi-account)
// would keep this running next to a live session.

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace bell
{
class BellHTTPServer;
} // namespace bell

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
    // (a message for the user) if the web server or the helper cannot start.
    static std::unique_ptr<Pairing> start(const std::string &device_name, std::string &error);

    // Stops advertising and the web server.
    ~Pairing();
    Pairing(const Pairing &) = delete;
    Pairing &operator=(const Pairing &) = delete;

    // The phone's login once it has arrived and decrypted, else nullptr.
    // Sets `error` if the phone sent something that could not be read.
    std::shared_ptr<cspot::LoginBlob> take_login(std::string &error);

  private:
    Pairing() = default;

    std::shared_ptr<cspot::LoginBlob> blob_;
    std::unique_ptr<bell::BellHTTPServer> server_;
    std::string info_json_; // GET reply, built once on the caller's thread

    std::mutex lock_; // guards query_ (web server thread -> caller)
    std::optional<std::map<std::string, std::string>> query_;
};

} // namespace spotify
