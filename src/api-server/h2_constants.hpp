// h2_constants.hpp
#pragma once
#include <cstddef>
#include <cstdint>

namespace h2 {

// ── HTTP/2 SETTINGS (sent to peer) ──
static constexpr uint32_t kMaxConcurrentStreams  = 1000;   // per connection
static constexpr uint32_t kInitialWindowSize     = 65535;  // RFC default
static constexpr uint32_t kMaxFrameSize          = 16384;  // RFC default

// ── Server-enforced limits ──
static constexpr size_t   kMaxHeaderListSize     = 65536;  // 64 KB decompressed
static constexpr size_t   kMaxRequestBodySize    = 1048576; // 1 MB per stream
static constexpr size_t   kMaxConnectionsPerWorker = 2500;

// ── Timeouts ──
static constexpr uint64_t kIdleTimeoutMs         = 30000;  // 30 seconds
static constexpr int      kGracefulShutdownTimeoutMs = 5000;
static constexpr int      kShutdownNoticeDelayMs = 1000;
static constexpr int      kDrainTimeoutMs        = 30000;

// ── Buffer sizes ──
static constexpr size_t   kReadBufSize           = 8192;   // 8 KB
static constexpr size_t   kWriteBufSize          = 65536;  // 64 KB

// ── Accept backlog ──
static constexpr int      kListenBacklog         = 128;  // explicit value, avoids <sys/socket.h> dependency

}  // namespace h2
