// ---------------------------------------------------------------------------
// test_stats_collector.cpp
//
// StatsCollector 단위 테스트.
//
// [테스트 범위]
// - 초기 상태 검증 (all-zero)
// - on_connection_open: total_connections / active_sessions 증가
// - on_connection_close: active_sessions 감소 (언더플로우 방지 포함)
// - on_query: total_queries / blocked_queries 증가
// - snapshot(): 반환 필드 및 block_rate 계산
// - snapshot(): qps 양수 확인 (elapsed > 0)
// - snapshot(): total == 0 시 block_rate == 0.0 (div-by-zero 방지)
// - ConcurrentAccess: 멀티스레드 동시성 (data race 미발생 확인)
//
// [스레드 안전성]
// StatsCollector 는 atomic 기반 헤더-온리 구현이므로 TSan 빌드에서
// 모든 동시성 테스트가 클린해야 한다.
//
// [알려진 한계]
// - qps 계산은 elapsed_sec 기반 누적 방식이므로 테스트 실행 시간에 따라
//   절대값이 달라진다. 양수 여부(> 0.0)만 검증한다.
// - block_rate 부동소수점 비교는 EXPECT_NEAR 으로 허용 오차 1e-9 이내.
// ---------------------------------------------------------------------------

#include "stats/stats_collector.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// InitialState_AllZero
//   생성 직후 모든 카운터가 0이어야 한다.
// ---------------------------------------------------------------------------
TEST(StatsCollector, InitialState_AllZero) {
    StatsCollector stats;
    const auto snap = stats.snapshot();

    EXPECT_EQ(snap.total_connections,  0u) << "total_connections must be 0 at init";
    EXPECT_EQ(snap.active_sessions,    0u) << "active_sessions must be 0 at init";
    EXPECT_EQ(snap.total_queries,      0u) << "total_queries must be 0 at init";
    EXPECT_EQ(snap.blocked_queries,    0u) << "blocked_queries must be 0 at init";
    EXPECT_NEAR(snap.block_rate,       0.0, 1e-9) << "block_rate must be 0.0 at init";
}

// ---------------------------------------------------------------------------
// OnConnectionOpen_IncrementsBoth
//   on_connection_open() 호출 시 total_connections 와 active_sessions 가
//   각각 1씩 증가해야 한다.
// ---------------------------------------------------------------------------
TEST(StatsCollector, OnConnectionOpen_IncrementsBoth) {
    StatsCollector stats;

    stats.on_connection_open();
    auto snap = stats.snapshot();
    EXPECT_EQ(snap.total_connections, 1u) << "total_connections should be 1 after one open";
    EXPECT_EQ(snap.active_sessions,   1u) << "active_sessions should be 1 after one open";

    stats.on_connection_open();
    snap = stats.snapshot();
    EXPECT_EQ(snap.total_connections, 2u) << "total_connections should increment to 2";
    EXPECT_EQ(snap.active_sessions,   2u) << "active_sessions should increment to 2";
}

// ---------------------------------------------------------------------------
// OnConnectionClose_DecrementsActive
//   on_connection_close() 호출 시 active_sessions 만 감소하고
//   total_connections 는 변하지 않아야 한다.
// ---------------------------------------------------------------------------
TEST(StatsCollector, OnConnectionClose_DecrementsActive) {
    StatsCollector stats;

    stats.on_connection_open();
    stats.on_connection_open();
    stats.on_connection_close();

    const auto snap = stats.snapshot();
    EXPECT_EQ(snap.total_connections, 2u)
        << "total_connections must not decrease on close";
    EXPECT_EQ(snap.active_sessions,   1u)
        << "active_sessions should decrease to 1 after one close";
}

// ---------------------------------------------------------------------------
// OnConnectionClose_NoUnderflow
//   active_sessions 가 0 인 상태에서 on_connection_close() 호출 시
//   언더플로우 없이 0을 유지해야 한다.
//   [엣지 케이스] atomic uint64_t 언더플로우 방지 로직 검증.
// ---------------------------------------------------------------------------
TEST(StatsCollector, OnConnectionClose_NoUnderflow) {
    StatsCollector stats;

    // 연결 열지 않고 닫기 시도
    stats.on_connection_close();

    const auto snap = stats.snapshot();
    EXPECT_EQ(snap.active_sessions, 0u)
        << "active_sessions must not underflow below 0";
}

// ---------------------------------------------------------------------------
// OnQuery_Blocked_IncrementsBlockedCount
//   on_query(true) 호출 시 blocked_queries 와 total_queries 가 모두 증가해야 한다.
// ---------------------------------------------------------------------------
TEST(StatsCollector, OnQuery_Blocked_IncrementsBlockedCount) {
    StatsCollector stats;

    stats.on_query(true);
    const auto snap = stats.snapshot();

    EXPECT_EQ(snap.total_queries,   1u) << "total_queries should be 1 after one blocked query";
    EXPECT_EQ(snap.blocked_queries, 1u) << "blocked_queries should be 1 after one blocked query";
}

// ---------------------------------------------------------------------------
// OnQuery_Allowed_IncrementsQueryCount
//   on_query(false) 호출 시 total_queries 만 증가하고
//   blocked_queries 는 변하지 않아야 한다.
// ---------------------------------------------------------------------------
TEST(StatsCollector, OnQuery_Allowed_IncrementsQueryCount) {
    StatsCollector stats;

    stats.on_query(false);
    const auto snap = stats.snapshot();

    EXPECT_EQ(snap.total_queries,   1u) << "total_queries should be 1 after one allowed query";
    EXPECT_EQ(snap.blocked_queries, 0u) << "blocked_queries should stay 0 for allowed query";
}

// ---------------------------------------------------------------------------
// Snapshot_BlockRate_Calculation
//   block_rate = blocked_queries / total_queries 계산이 올바른지 검증한다.
//   허용 2건 + 차단 2건 → block_rate == 0.5.
// ---------------------------------------------------------------------------
TEST(StatsCollector, Snapshot_BlockRate_Calculation) {
    StatsCollector stats;

    stats.on_query(false);  // 허용
    stats.on_query(false);  // 허용
    stats.on_query(true);   // 차단
    stats.on_query(true);   // 차단

    const auto snap = stats.snapshot();

    EXPECT_EQ(snap.total_queries,   4u) << "total_queries should be 4";
    EXPECT_EQ(snap.blocked_queries, 2u) << "blocked_queries should be 2";
    EXPECT_NEAR(snap.block_rate, 0.5, 1e-9)
        << "block_rate should be 0.5 (2 blocked / 4 total)";
}

// ---------------------------------------------------------------------------
// Snapshot_BlockRate_AllBlocked
//   모든 쿼리가 차단된 경우 block_rate == 1.0 이어야 한다.
// ---------------------------------------------------------------------------
TEST(StatsCollector, Snapshot_BlockRate_AllBlocked) {
    StatsCollector stats;

    stats.on_query(true);
    stats.on_query(true);
    stats.on_query(true);

    const auto snap = stats.snapshot();
    EXPECT_NEAR(snap.block_rate, 1.0, 1e-9)
        << "block_rate should be 1.0 when all queries are blocked";
}

// ---------------------------------------------------------------------------
// Snapshot_BlockRate_ZeroTotal
//   total_queries == 0 일 때 block_rate == 0.0 이어야 한다 (div-by-zero 방지).
// ---------------------------------------------------------------------------
TEST(StatsCollector, Snapshot_BlockRate_ZeroTotal) {
    StatsCollector stats;
    const auto snap = stats.snapshot();

    EXPECT_NEAR(snap.block_rate, 0.0, 1e-9)
        << "block_rate should be 0.0 when total_queries == 0 (no div-by-zero)";
}

// ---------------------------------------------------------------------------
// Snapshot_CapturedAt_IsSet
//   snapshot() 이 반환하는 captured_at 이 epoch 보다 크고 현재 시각보다
//   작아야 한다 (타임스탬프가 설정됨을 확인).
// ---------------------------------------------------------------------------
TEST(StatsCollector, Snapshot_CapturedAt_IsSet) {
    using clock = std::chrono::system_clock;
    const auto before = clock::now();

    StatsCollector stats;
    const auto snap = stats.snapshot();

    const auto after = clock::now();

    EXPECT_GE(snap.captured_at, before) << "captured_at should be >= before snapshot call";
    EXPECT_LE(snap.captured_at, after)  << "captured_at should be <= after snapshot call";
}

// ---------------------------------------------------------------------------
// Snapshot_Qps_PositiveAfterQuery
//   쿼리를 실행한 후 qps 가 0 보다 커야 한다.
//   (elapsed_sec > 0 이고 window_queries > 0 이면 qps > 0)
// ---------------------------------------------------------------------------
TEST(StatsCollector, Snapshot_Qps_PositiveAfterQuery) {
    StatsCollector stats;
    stats.on_query(false);

    const auto snap = stats.snapshot();
    EXPECT_GT(snap.qps, 0.0)
        << "qps should be > 0 after at least one query";
}

// ---------------------------------------------------------------------------
// ConcurrentAccess_NoDataRace
//   여러 스레드에서 동시에 on_connection_open / on_query / snapshot 을
//   호출해도 data race 없이 안전해야 한다.
//
//   [TSan 검증 포인트]
//   TSan 빌드에서 실행 시 이 테스트에서 data race 가 발생하면 안 된다.
//
//   [검증 방법]
//   - N개 스레드가 각각 M번 on_connection_open / on_query 호출
//   - 별도 reader 스레드가 동시에 snapshot() 호출
//   - 모든 스레드 종료 후 total_connections / total_queries 합계 검증
// ---------------------------------------------------------------------------
TEST(StatsCollector, ConcurrentAccess_NoDataRace) {
    StatsCollector stats;

    constexpr int kWriterThreads = 4;
    constexpr int kOpsPerThread  = 1000;

    std::vector<std::thread> writers;
    writers.reserve(static_cast<std::size_t>(kWriterThreads));

    // writer 스레드: on_connection_open + on_query 반복
    for (int i = 0; i < kWriterThreads; ++i) {
        writers.emplace_back([&stats, i]() {
            for (int j = 0; j < kOpsPerThread; ++j) {
                stats.on_connection_open();
                stats.on_query(j % 2 == 0);  // 홀짝으로 차단/허용 교번
            }
            // 열었던 연결 닫기
            for (int j = 0; j < kOpsPerThread; ++j) {
                stats.on_connection_close();
            }
            (void)i;
        });
    }

    // reader 스레드: snapshot() 반복 (data race 유발 여부 확인)
    std::atomic<bool> stop_reader{false};
    std::thread reader([&stats, &stop_reader]() {
        while (!stop_reader.load(std::memory_order_relaxed)) {
            const auto snap = stats.snapshot();
            // snapshot 값이 합리적인 범위 내여야 함 (음수 방지)
            EXPECT_LE(snap.active_sessions, snap.total_connections)
                << "active_sessions cannot exceed total_connections";
        }
    });

    for (auto& t : writers) { t.join(); }
    stop_reader.store(true, std::memory_order_relaxed);
    reader.join();

    // 최종 집계 검증
    const auto snap = stats.snapshot();
    const auto expected_total =
        static_cast<std::uint64_t>(kWriterThreads) *
        static_cast<std::uint64_t>(kOpsPerThread);

    EXPECT_EQ(snap.total_connections, expected_total)
        << "total_connections must equal kWriterThreads * kOpsPerThread";
    EXPECT_EQ(snap.total_queries, expected_total)
        << "total_queries must equal kWriterThreads * kOpsPerThread";

    // 모든 연결을 닫았으므로 active_sessions == 0
    EXPECT_EQ(snap.active_sessions, 0u)
        << "active_sessions should be 0 after all closes";

    // block_rate: 홀짝 교번 → 절반 차단 → 0.5 근방
    EXPECT_NEAR(snap.block_rate, 0.5, 0.01)
        << "block_rate should be ~0.5 with alternating block/allow";
}
