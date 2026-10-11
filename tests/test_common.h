#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>

// Explicit runtime check (never compiled out by NDEBUG): prints a diagnostic
// and terminates with exit code 1, so that ctest reports the failure.
#define CHECK(cond, msg)                                                   \
  do {                                                                     \
    if (!(cond)) {                                                         \
      printf("FAIL: %s (%s:%d)\n", std::string(msg).c_str(), __FILE__,     \
             __LINE__);                                                    \
      fflush(stdout);                                                      \
      _Exit(1);                                                            \
    }                                                                      \
  } while (0)

#define TEST_EXIT_OK() \
  do {                 \
    printf("OK\n");    \
    fflush(stdout);    \
    _Exit(0);          \
  } while (0)

/// exit code 77 is reported by ctest as skipped (SKIP_RETURN_CODE)
#define TEST_SKIP(msg)               \
  do {                               \
    printf("SKIPPED: %s\n", msg);    \
    fflush(stdout);                  \
    _Exit(77);                       \
  } while (0)

/// runs a shell command and returns its exit code
inline int run(const std::string& cmd) {
  int rc = system((cmd + " >/dev/null 2>&1").c_str());
  if (rc == -1) return -1;
  return WEXITSTATUS(rc);
}

inline bool haveTool(const char* tool) {
  return run(std::string("(command -v ") + tool + " || test -x /usr/sbin/" + tool +
             " || test -x /sbin/" + tool + ")") == 0;
}

/// path with /usr/sbin and /sbin, where the e2fsprogs live
inline std::string tool(const char* name) {
  return std::string("PATH=$PATH:/usr/sbin:/sbin ") + name;
}

/// deterministic test data
inline uint8_t pattern(uint64_t pos, uint32_t seed) {
  uint64_t x = pos * 2654435761u + seed * 40503u;
  return (uint8_t)(x ^ (x >> 13) ^ (x >> 7));
}
