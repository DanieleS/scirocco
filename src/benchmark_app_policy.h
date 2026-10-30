/**
 * @file src/benchmark_app_policy.h
 * @brief How /applist treats the hidden built-in Benchmark app.
 */
#pragma once

namespace nvhttp {
  /**
   * @brief Whether /applist shows an app, as far as the hidden Benchmark app is concerned.
   *
   * The Benchmark app stays out of the app grid: clients launch it by the UUID serverinfo gives.
   * Except while it runs, so that a benchmark cut short can still be resumed or quit from any
   * client, including one that does not know about it.
   *
   * @param appid The app being listed.
   * @param current_appid The running app, 0 when none.
   * @param benchmark_appid The Benchmark app's id, -1 when the host does not offer it.
   * @return False only for the Benchmark app while it is not running.
   */
  inline bool benchmark_app_listed(int appid, int current_appid, int benchmark_appid) {
    return benchmark_appid <= 0 || appid != benchmark_appid || appid == current_appid;
  }
}  // namespace nvhttp
