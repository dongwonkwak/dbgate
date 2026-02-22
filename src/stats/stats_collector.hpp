#pragma once

// ---------------------------------------------------------------------------
// stats_collector.hpp
//
// 실시간 통계 수집기. 헤더 전용 (atomic inline 구현).
//
// [스레드 안전성]
// - on_connection_open / on_connection_close / on_query:
//   데이터패스에서 concurrent 호출 안전 (atomic 사용).
// - snapshot():
//   조회 경로에서 호출. 갱신 경로와 contention 없이 읽기 가능.
//
// [격리 원칙]
// - 통계 수집 실패가 데이터패스 실패로 전파되지 않도록
//   모든 갱신 메서드는 noexcept 로 선언한다.
//
// [읽기/쓰기 경로 분리]
// - 갱신 경로: on_connection_open / on_connection_close / on_query
// - 조회 경로: snapshot()
//   두 경로 간 mutex 없이 atomic 로드/스토어로 분리한다.
// ---------------------------------------------------------------------------

#include "common/types.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>

// ---------------------------------------------------------------------------
// StatsSnapshot
//   특정 시점의 통계 스냅샷 (불변 값 객체).
//   qps      : 1초 슬라이딩 윈도우 기반 초당 쿼리 수
//   block_rate: blocked_queries / total_queries (total == 0 이면 0.0)
// ---------------------------------------------------------------------------
struct StatsSnapshot {
    std::uint64_t                              total_connections{0};
    std::uint64_t                              active_sessions{0};
    std::uint64_t                              total_queries{0};
    std::uint64_t                              blocked_queries{0};
    double                                     qps{0.0};
    double                                     block_rate{0.0};
    std::chrono::system_clock::time_point      captured_at{};
};

// ---------------------------------------------------------------------------
// StatsCollector
//   데이터패스 이벤트를 집계하고 StatsSnapshot 을 제공한다.
//
// [QPS 계산 방식 (Phase 3 구현)]
//   1초 슬라이딩 윈도우를 atomic timestamp + counter 로 구현.
//   현재 stub 에서는 simple counter / elapsed 방식을 사용한다.
// ---------------------------------------------------------------------------
class StatsCollector {
public:
    StatsCollector() noexcept
        : total_connections_{0}
        , active_sessions_{0}
        , total_queries_{0}
        , blocked_queries_{0}
        , window_queries_{0}
        , window_start_(std::chrono::system_clock::now())
    {}

    ~StatsCollector() = default;

    // 복사 금지 (atomic 은 복사 불가)
    StatsCollector(const StatsCollector&)            = delete;
    StatsCollector& operator=(const StatsCollector&) = delete;

    // 이동 금지 (atomic 소유권 명확화)
    StatsCollector(StatsCollector&&)            = delete;
    StatsCollector& operator=(StatsCollector&&) = delete;

    // on_connection_open
    //   새 클라이언트 연결 수립 시 호출 (데이터패스).
    //   [noexcept] 통계 실패가 데이터패스로 전파되지 않도록.
    void on_connection_open() noexcept {
        total_connections_.fetch_add(1, std::memory_order_relaxed);
        active_sessions_.fetch_add(1, std::memory_order_relaxed);
    }

    // on_connection_close
    //   클라이언트 연결 종료 시 호출 (데이터패스).
    void on_connection_close() noexcept {
        const std::uint64_t current = active_sessions_.load(std::memory_order_relaxed);
        if (current > 0) {
            active_sessions_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    // on_query
    //   쿼리 처리 완료 시 호출 (데이터패스).
    //   blocked: 정책에 의해 차단된 쿼리면 true
    void on_query(bool blocked) noexcept {
        total_queries_.fetch_add(1, std::memory_order_relaxed);
        window_queries_.fetch_add(1, std::memory_order_relaxed);
        if (blocked) {
            blocked_queries_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // snapshot
    //   현재 통계의 불변 스냅샷을 반환한다 (조회 경로).
    //
    //   [QPS 계산]
    //   window_start_ 이후 경과 시간과 window_queries_ 를 이용해 계산.
    //   Phase 3 에서 1초 슬라이딩 윈도우로 교체 예정.
    [[nodiscard]] StatsSnapshot snapshot() const noexcept {
        const auto now          = std::chrono::system_clock::now();
        const auto total_conn   = total_connections_.load(std::memory_order_relaxed);
        const auto active_sess  = active_sessions_.load(std::memory_order_relaxed);
        const auto total_q      = total_queries_.load(std::memory_order_relaxed);
        const auto blocked_q    = blocked_queries_.load(std::memory_order_relaxed);
        const auto window_q     = window_queries_.load(std::memory_order_relaxed);
        const auto window_start = window_start_.load();

        const double elapsed_sec = std::chrono::duration<double>(
            now - window_start).count();

        double qps = 0.0;
        if (elapsed_sec > 0.0) {
            qps = static_cast<double>(window_q) / elapsed_sec;
        }

        double block_rate = 0.0;
        if (total_q > 0) {
            block_rate = static_cast<double>(blocked_q) / static_cast<double>(total_q);
        }

        return StatsSnapshot{
            .total_connections = total_conn,
            .active_sessions   = active_sess,
            .total_queries     = total_q,
            .blocked_queries   = blocked_q,
            .qps               = qps,
            .block_rate        = block_rate,
            .captured_at       = now,
        };
    }

private:
    std::atomic<std::uint64_t>                              total_connections_;
    std::atomic<std::uint64_t>                              active_sessions_;
    std::atomic<std::uint64_t>                              total_queries_;
    std::atomic<std::uint64_t>                              blocked_queries_;

    // QPS 슬라이딩 윈도우용 카운터/타임스탬프
    // Phase 3 에서 1초 윈도우 교체 시 ring buffer 방식으로 변경 예정.
    std::atomic<std::uint64_t>                              window_queries_;
    std::atomic<std::chrono::system_clock::time_point>      window_start_;
};
