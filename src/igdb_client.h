/**
 * @file src/igdb_client.h
 * @brief Runtime half of the IGDB integration: credentials, tokens, requests, cache, art.
 */
#pragma once

// standard includes
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// local includes
#include "game_metadata.h"
#include "igdb_policy.h"

namespace igdb {

  /// @brief What the settings page needs to show, without ever returning the secret itself.
  struct status_t {
    bool enabled {false};
    // Both a client id and a stored secret are present. Says nothing about whether they work.
    bool configured {false};
    // A token was obtained at least once this run and has not been rejected since.
    bool authenticated {false};
    // Why the last request or token exchange failed, for the settings page to show verbatim.
    std::string last_error;
    // Where the secret is read from and written to, after every default and fallback has been
    // applied. Shown in the settings, because "it is not picking up my secret" is otherwise a
    // guess about a path nobody can see.
    std::string secret_file;
  };

  status_t status();

  /**
   * @brief Store the client secret outside sunshine.conf.
   *
   * GET /api/config returns the config file as it stands, so a secret kept there would be
   * handed to anything allowed to read settings. This writes it to its own owner-only file,
   * the same way host credentials are kept.
   */
  bool save_secret(const std::string &secret, std::string &error_out);

  /// @brief Forget the stored secret. Leaves the client id alone; that one is not sensitive.
  bool clear_secret(std::string &error_out);

  /// @brief Exchange the stored credentials for a token now, to tell the user they work.
  bool verify(std::string &error_out);

  /// @brief Fetch one IGDB record, from the on-disk cache when it is still fresh.
  std::optional<policy::game_t> fetch_game(const std::string &igdb_id, std::string &error_out);

  /**
   * @brief Fetch many IGDB records, keyed by id, a few hundred per request.
   *
   * Fresh cache entries cost nothing; the rest go out in chunks of policy::k_bulk_chunk_size.
   * An id missing from the result is one IGDB does not have, unless `error_out` is set: the
   * first failed chunk stops the fetch, because whatever failed it (a token, a rate limit, the
   * network) would fail the next one too.
   */
  std::unordered_map<std::string, policy::game_t> fetch_games(const std::vector<std::string> &igdb_ids, std::string &error_out);

  /// @brief Search IGDB by name. Used by auto-matching and by the manual picker.
  std::vector<policy::game_t> search(const std::string &name, int limit, std::string &error_out);

  /**
   * @brief Search for many titles, ten to a request through IGDB's multiquery endpoint.
   *
   * One slot per title, in order. A slot is nullopt when its search never ran or IGDB did not
   * answer it, and an empty list when the search ran and found nothing. Stops at the first
   * failed request, with `error_out` set.
   */
  std::vector<std::optional<std::vector<policy::game_t>>> search_many(const std::vector<std::string> &names,
                                                                      int limit,
                                                                      std::string &error_out);

  /**
   * @brief Find the IGDB game a set of store ids belongs to.
   *
   * Returns the id of the first store IGDB recognised. Empty when none of the ids belong to an
   * indexed store or IGDB knows none of them -- both are ordinary outcomes, not errors.
   */
  std::string resolve_store_ids(const std::vector<metadata::store_id_t> &ids, std::string &error_out);

  /**
   * @brief resolve_store_ids for a whole library, a few hundred store ids per request.
   *
   * One slot per game, in order: the IGDB id, empty when IGDB knows none of the game's store
   * ids, or nullopt when the request that would have answered for it failed (`error_out` is
   * then set). The first failure stops the lookup.
   */
  std::vector<std::optional<std::string>> resolve_store_ids_bulk(
    const std::vector<std::vector<metadata::store_id_t>> &ids_per_game,
    std::string &error_out
  );

  /**
   * @brief Download a record's cover and hero art into the managed covers directory.
   *
   * Returns the local path, or empty when the record has no such image or the download failed.
   * An existing file is reused, so a re-resolve costs nothing.
   */
  std::string download_cover(const policy::game_t &game, const std::filesystem::path &covers_root);
  std::string download_background(const policy::game_t &game, const std::filesystem::path &covers_root);

  /**
   * @brief Drop every cached IGDB record. Returns how many files went.
   *
   * Downloaded covers and hero art stay: apps.json points at them, and removing them would
   * leave those apps showing placeholder art until each one was resolved again.
   */
  std::size_t clear_cache();

}  // namespace igdb
