#include "AccessKeyFetcher.h"

#include <sys/utsname.h>     // for uname
#include <chrono>            // for steady_clock
#include <cstring>           // for strncpy
#include <initializer_list>  // for initializer_list
#include <vector>            // for vector

#include <mbedtls/sha1.h>  // for mbedtls_sha1

#include "BellLogger.h"    // for AbstractLogger
#include "BellUtils.h"     // for BELL_SLEEP_MS
#include "CSpotContext.h"  // for Context
#include "HTTPClient.h"
#include "Logger.h"        // for CSPOT_LOG
#include "TimeProvider.h"  // for TimeProvider
#include "pb_decode.h"     // for pb_decode, pb_release
#include "pb_encode.h"     // for pb_encode
#include "protobuf/clienttoken.pb.h"
#include "protobuf/login5.pb.h"

using namespace cspot;

// Desktop client version librespot 0.8.0 reports to clienttoken.
static const char* CLIENT_VERSION = "1.2.52.442";
static const char* USER_AGENT = "Spotify/124200290 Linux/0 (cspot)";
static const int MAX_TRIES = 3;

// Encodes a nanopb struct; returns empty on failure.
static std::vector<uint8_t> encode(const pb_msgdesc_t* fields,
                                   const void* src) {
  size_t size = 0;
  if (!pb_get_encoded_size(&size, fields, src)) {
    return {};
  }
  std::vector<uint8_t> out(size);
  pb_ostream_t stream = pb_ostream_from_buffer(out.data(), out.size());
  if (!pb_encode(&stream, fields, src)) {
    return {};
  }
  return out;
}

// Decodes into a zeroed nanopb struct; false on empty or malformed data.
static bool decode(const pb_msgdesc_t* fields, void* dst,
                   const std::vector<uint8_t>& data) {
  if (data.empty()) {
    return false;
  }
  pb_istream_t stream = pb_istream_from_buffer(data.data(), data.size());
  if (!pb_decode(&stream, fields, dst)) {
    CSPOT_LOG(error, "protobuf decode failed: %s", PB_GET_ERROR(&stream));
    return false;
  }
  return true;
}

static uint64_t readBE64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) {
    v = (v << 8) | p[i];
  }
  return v;
}

static void writeBE64(uint64_t v, uint8_t* p) {
  for (int i = 7; i >= 0; i--) {
    p[i] = v & 0xff;
    v >>= 8;
  }
}

/**
 * Spotify hashcash, ported from librespot core/src/util.rs solve_hash_cash.
 * Finds a 16-byte suffix so that SHA1(prefix || suffix) has `length` trailing
 * zero bits in bytes 12..20. `ctx` seeds the starting counter.
 * Gives up after 5 seconds, like librespot (the challenge expires).
 */
static bool solveHashcash(const std::vector<uint8_t>& ctx,
                          const uint8_t* prefix, size_t prefixLen, int length,
                          uint8_t suffix[16], int64_t& elapsedNs) {
  uint8_t md[20];
  mbedtls_sha1(ctx.data(), ctx.size(), md);
  uint64_t target = readBE64(md + 12);

  std::vector<uint8_t> buf(prefix, prefix + prefixLen);
  buf.resize(prefixLen + 16);

  auto start = std::chrono::steady_clock::now();
  for (uint64_t counter = 0;; counter++) {
    if ((counter & 0xffff) == 0 &&
        std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
      return false;
    }
    writeBE64(target + counter, &buf[prefixLen]);
    writeBE64(counter, &buf[prefixLen + 8]);
    mbedtls_sha1(buf.data(), buf.size(), md);
    uint64_t value = readBE64(md + 12);
    int zeros = value == 0 ? 64 : __builtin_ctzll(value);
    if (zeros >= length) {
      memcpy(suffix, &buf[prefixLen], 16);
      elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
      return true;
    }
  }
}

static std::string toUpperHex(const uint8_t* data, size_t len) {
  static const char* digits = "0123456789ABCDEF";
  std::string out;
  for (size_t i = 0; i < len; i++) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 0xf];
  }
  return out;
}

static std::vector<uint8_t> fromHex(const char* hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
    out.push_back((uint8_t)strtol(std::string(hex + i, 2).c_str(), nullptr, 16));
  }
  return out;
}

static void copyString(char* dst, size_t dstSize, const std::string& src) {
  strncpy(dst, src.c_str(), dstSize - 1);
  dst[dstSize - 1] = '\0';
}

static std::vector<uint8_t> postProtobuf(
    const std::string& url, const std::vector<uint8_t>& body,
    const std::string& clientToken = "") {
  bell::HTTPClient::Headers headers = {
      {"Content-Type", "application/x-protobuf"},
      {"Accept", "application/x-protobuf"},
      {"User-Agent", USER_AGENT}};
  if (!clientToken.empty()) {
    headers.push_back({"client-token", clientToken});
  }
  return bell::HTTPClient::post(url, headers, body)->bytes();
}

AccessKeyFetcher::AccessKeyFetcher(std::shared_ptr<cspot::Context> ctx)
    : ctx(ctx) {}

bool AccessKeyFetcher::isExpired() {
  if (accessKey.empty()) {
    return true;
  }

  if (ctx->timeProvider->getSyncedTimestamp() > expiresAt) {
    return true;
  }

  return false;
}

std::string AccessKeyFetcher::getAccessKey() {
  if (!isExpired()) {
    return accessKey;
  }

  updateAccessKey();

  return accessKey;
}

/**
 * Gets a client token from clienttoken.spotify.com, identifying as a Linux
 * desktop client (same as librespot on Linux). Solves the hashcash challenge
 * if the server sends one.
 */
std::string AccessKeyFetcher::getClientToken() {
  return updateClientToken() ? clientToken : std::string();
}

bool AccessKeyFetcher::updateClientToken() {
  if (!clientToken.empty() &&
      ctx->timeProvider->getSyncedTimestamp() < clientTokenExpiresAt) {
    return true;
  }

  static const char* URL = "https://clienttoken.spotify.com/v1/clienttoken";

  ClientTokenRequest request = ClientTokenRequest_init_zero;
  request.has_request_type = true;
  request.request_type = ClientTokenRequestType_REQUEST_CLIENT_DATA_REQUEST;
  request.which_request = ClientTokenRequest_client_data_tag;
  auto& data = request.request.client_data;
  data.has_client_version = true;
  copyString(data.client_version, sizeof(data.client_version), CLIENT_VERSION);
  data.has_client_id = true;
  copyString(data.client_id, sizeof(data.client_id), ctx->config.clientId);
  data.which_data = ClientDataRequest_connectivity_sdk_data_tag;
  auto& sdk = data.data.connectivity_sdk_data;
  sdk.has_device_id = true;
  copyString(sdk.device_id, sizeof(sdk.device_id), ctx->config.deviceId);
  sdk.has_platform_specific_data = true;
  sdk.platform_specific_data.which_data =
      PlatformSpecificData_desktop_linux_tag;
  auto& desktop = sdk.platform_specific_data.data.desktop_linux;
  struct utsname uts = {};
  uname(&uts);
  desktop.has_system_name = true;
  copyString(desktop.system_name, sizeof(desktop.system_name), "Linux");
  desktop.has_system_release = true;
  copyString(desktop.system_release, sizeof(desktop.system_release), uts.release);
  desktop.has_system_version = true;
  copyString(desktop.system_version, sizeof(desktop.system_version), uts.version);
  desktop.has_hardware = true;
  copyString(desktop.hardware, sizeof(desktop.hardware), uts.machine);

  auto body = encode(ClientTokenRequest_fields, &request);

  for (int attempt = 1; attempt <= MAX_TRIES; attempt++) {
    ClientTokenResponse response = ClientTokenResponse_init_zero;
    if (!decode(ClientTokenResponse_fields, &response,
                postProtobuf(URL, body))) {
      CSPOT_LOG(error, "Client token: bad response (%d/%d)", attempt,
                MAX_TRIES);
      continue;
    }

    if (response.which_response == ClientTokenResponse_granted_token_tag &&
        response.response.granted_token.token) {
      auto& granted = response.response.granted_token;
      clientToken = granted.token;
      int expiresIn = granted.has_expires_after_seconds
                          ? granted.expires_after_seconds
                          : 3600;
      clientTokenExpiresAt =
          ctx->timeProvider->getSyncedTimestamp() + expiresIn * 1000LL;
      CSPOT_LOG(info, "Client token fetched");
      pb_release(ClientTokenResponse_fields, &response);
      return true;
    }

    if (response.which_response == ClientTokenResponse_challenges_tag &&
        response.response.challenges.challenges_count > 0) {
      auto& challenges = response.response.challenges;
      auto& challenge = challenges.challenges[0];
      if (challenge.which_parameters !=
              ClientTokenChallenge_evaluate_hashcash_parameters_tag ||
          !challenge.parameters.evaluate_hashcash_parameters.prefix) {
        CSPOT_LOG(error, "Client token: unsupported challenge type %d",
                  challenge.type);
        pb_release(ClientTokenResponse_fields, &response);
        return false;
      }

      auto& params = challenge.parameters.evaluate_hashcash_parameters;
      auto prefix = fromHex(params.prefix);
      uint8_t suffix[16];
      int64_t elapsedNs = 0;
      if (solveHashcash({}, prefix.data(), prefix.size(), params.length,
                        suffix, elapsedNs)) {
        ClientTokenRequest answer = ClientTokenRequest_init_zero;
        answer.has_request_type = true;
        answer.request_type =
            ClientTokenRequestType_REQUEST_CHALLENGE_ANSWERS_REQUEST;
        answer.which_request = ClientTokenRequest_challenge_answers_tag;
        auto& answers = answer.request.challenge_answers;
        answers.state = challenges.state;  // borrowed until pb_release below
        answers.answers_count = 1;
        answers.answers[0].has_ChallengeType = true;
        answers.answers[0].ChallengeType =
            ClientTokenChallengeType_CHALLENGE_HASH_CASH;
        answers.answers[0].which_answer = ChallengeAnswer_hash_cash_tag;
        answers.answers[0].answer.hash_cash.has_suffix = true;
        copyString(answers.answers[0].answer.hash_cash.suffix,
                   sizeof(answers.answers[0].answer.hash_cash.suffix),
                   toUpperHex(suffix, 16));
        body = encode(ClientTokenRequest_fields, &answer);
        pb_release(ClientTokenResponse_fields, &response);
        // Next loop iteration posts the answer
        continue;
      }
      CSPOT_LOG(error, "Client token: hashcash not solved in time");
    }

    pb_release(ClientTokenResponse_fields, &response);
    // Start over with a fresh client data request
    body = encode(ClientTokenRequest_fields, &request);
  }

  CSPOT_LOG(error, "Failed to fetch client token");
  return false;
}

/**
 * Gets an access token from login5 using the stored (reusable) credentials
 * of the current session. Same flow as librespot core/src/login5.rs
 * auth_token(): client-token header, hashcash challenges, up to 3 tries.
 * ctx->config.clientId must be the client id the credentials belong to.
 */
void AccessKeyFetcher::updateAccessKey() {
  if (keyPending) {
    // Already pending refresh request
    return;
  }

  keyPending = true;
  CSPOT_LOG(info, "Access token expired, fetching new one...");

  static const char* URL = "https://login5.spotify.com/v3/login";

  if (!updateClientToken()) {
    keyPending = false;
    return;
  }

  Login5Request request = Login5Request_init_zero;
  request.has_client_info = true;
  copyString(request.client_info.client_id,
             sizeof(request.client_info.client_id), ctx->config.clientId);
  request.client_info.has_client_id = true;
  copyString(request.client_info.device_id,
             sizeof(request.client_info.device_id), ctx->config.deviceId);
  request.client_info.has_device_id = true;
  request.which_login_method = Login5Request_stored_credential_tag;
  auto& stored = request.login_method.stored_credential;
  stored.has_username = true;
  copyString(stored.username, sizeof(stored.username), ctx->config.username);
  if (ctx->config.authData.size() > sizeof(stored.data.bytes)) {
    CSPOT_LOG(error, "Stored credentials too large (%d bytes)",
              (int)ctx->config.authData.size());
    keyPending = false;
    return;
  }
  stored.has_data = true;
  stored.data.size = ctx->config.authData.size();
  memcpy(stored.data.bytes, ctx->config.authData.data(),
         ctx->config.authData.size());

  // Holds the previous response while `request` borrows its login_context
  Login5Response previous = Login5Response_init_zero;
  bool success = false;

  for (int attempt = 1; attempt <= MAX_TRIES && !success; attempt++) {
    auto body = encode(Login5Request_fields, &request);
    auto bytes = postProtobuf(URL, body, clientToken);

    // request no longer needs the borrowed login_context
    request.login_context = nullptr;
    pb_release(Login5Response_fields, &previous);
    previous = Login5Response_init_zero;

    Login5Response response = Login5Response_init_zero;
    if (!decode(Login5Response_fields, &response, bytes)) {
      CSPOT_LOG(error, "login5: bad response (%d/%d)", attempt, MAX_TRIES);
      continue;
    }

    if (response.which_response == Login5Response_ok_tag &&
        response.response.ok.access_token) {
      accessKey = response.response.ok.access_token;
      int expiresIn = response.response.ok.has_access_token_expires_in
                          ? response.response.ok.access_token_expires_in
                          : 3600;
      // Refresh at half the lifetime
      expiresAt =
          ctx->timeProvider->getSyncedTimestamp() + expiresIn * 1000LL / 2;
      CSPOT_LOG(info, "Access token sucessfully fetched");
      success = true;
    } else if (response.which_response == Login5Response_error_tag) {
      auto error = response.response.error;
      CSPOT_LOG(error, "login5 error %d (%d/%d)", (int)error, attempt,
                MAX_TRIES);
      if (error != Login5Error_LOGIN5_TIMEOUT &&
          error != Login5Error_LOGIN5_TOO_MANY_ATTEMPTS &&
          error != Login5Error_LOGIN5_TRY_AGAIN_LATER) {
        pb_release(Login5Response_fields, &response);
        break;
      }
      // Back off instead of hammering, like librespot's LOGIN_TIMEOUT
      BELL_SLEEP_MS(3000 * attempt);
    } else if (response.which_response == Login5Response_challenges_tag) {
      auto& challenges = response.response.challenges;
      std::vector<uint8_t> loginContext;
      if (response.login_context) {
        loginContext.assign(
            response.login_context->bytes,
            response.login_context->bytes + response.login_context->size);
      }

      request.has_challenge_solutions = true;
      request.challenge_solutions.solutions_count = 0;
      bool solvable = true;
      for (pb_size_t i = 0; i < challenges.challenges_count; i++) {
        auto& challenge = challenges.challenges[i];
        if (challenge.which_challenge == Login5Challenge_code_tag) {
          CSPOT_LOG(error, "login5: code (SMS) challenge is not supported");
          solvable = false;
          break;
        }
        if (challenge.which_challenge != Login5Challenge_hashcash_tag ||
            !challenge.challenge.hashcash.prefix) {
          continue;  // empty challenge, skip like librespot
        }
        auto& hashcash = challenge.challenge.hashcash;
        auto& solution = request.challenge_solutions
                             .solutions[request.challenge_solutions
                                            .solutions_count++];
        solution = Login5ChallengeSolution_init_zero;
        solution.which_solution = Login5ChallengeSolution_hashcash_tag;
        int64_t elapsedNs = 0;
        if (!solveHashcash(loginContext, hashcash.prefix->bytes,
                           hashcash.prefix->size, hashcash.length,
                           solution.solution.hashcash.suffix.bytes,
                           elapsedNs)) {
          CSPOT_LOG(error, "login5: hashcash not solved in time");
          solvable = false;
          break;
        }
        solution.solution.hashcash.has_suffix = true;
        solution.solution.hashcash.suffix.size = 16;
        solution.solution.hashcash.has_duration = true;
        solution.solution.hashcash.duration.has_seconds = true;
        solution.solution.hashcash.duration.seconds = elapsedNs / 1000000000;
        solution.solution.hashcash.duration.has_nanos = true;
        solution.solution.hashcash.duration.nanos = elapsedNs % 1000000000;
        CSPOT_LOG(debug, "login5: solved hashcash in %lld ms",
                  (long long)(elapsedNs / 1000000));
      }
      if (!solvable) {
        pb_release(Login5Response_fields, &response);
        break;
      }
      // Borrow the server's login_context for the next request
      request.login_context = response.login_context;
      previous = response;
      continue;
    } else {
      CSPOT_LOG(error, "login5: unexpected response (%d/%d)", attempt,
                MAX_TRIES);
    }

    pb_release(Login5Response_fields, &response);
  }

  request.login_context = nullptr;
  pb_release(Login5Response_fields, &previous);

  if (!success) {
    CSPOT_LOG(error, "Failed to fetch access token");
  }

  keyPending = false;
}
