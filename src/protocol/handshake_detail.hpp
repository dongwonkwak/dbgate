#pragma once

// ---------------------------------------------------------------------------
// handshake_detail.hpp  —  테스트 전용 내부 인터페이스
//
// 이 헤더는 프로덕션 코드가 아닌 단위 테스트에서만 include한다.
// handshake.cpp 내부 detail namespace의 순수 함수를 노출한다.
//
// 주의: 공개 API(handshake.hpp)를 변경하지 않고 테스트 가능성만 확보한다.
//       프로덕션 바이너리(dbgate)는 이 헤더를 포함하지 않는다.
// ---------------------------------------------------------------------------

#include "common/types.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace detail {

// ---------------------------------------------------------------------------
// AuthResponseType
//   MySQL 핸드셰이크 auth 응답 패킷의 첫 바이트 + payload 길이로
//   패킷 종류를 분류한다.
// ---------------------------------------------------------------------------
enum class AuthResponseType : std::uint8_t {
    kOk,           // 0x00 — 핸드셰이크 완료
    kError,        // 0xFF — 인증 실패
    kEof,          // 0xFE + payload < 9 — 핸드셰이크 실패
    kAuthSwitch,   // 0xFE + payload >= 9 — AuthSwitchRequest (추가 라운드트립)
    kAuthMoreData, // 0x01 — caching_sha2_password 등 추가 라운드트립
    kUnknown,      // 그 외 — fail-close 처리
};

// ---------------------------------------------------------------------------
// HandshakeState
//   relay_handshake() 내부 상태 머신의 상태를 나타낸다.
// ---------------------------------------------------------------------------
enum class HandshakeState : std::uint8_t {
    kWaitServerGreeting,    // 초기: 서버 Initial Handshake 대기
    kWaitClientResponse,    // 클라이언트 HandshakeResponse 대기
    kWaitServerAuth,        // 서버 첫 번째 auth 응답 대기
    kWaitClientAuthSwitch,  // AuthSwitch 후 클라이언트 응답 대기
    kWaitServerAuthSwitch,  // AuthSwitch 후 서버 응답 대기
    kWaitClientMoreData,    // AuthMoreData 후 클라이언트 응답 대기
    kWaitServerMoreData,    // AuthMoreData 후 서버 응답 대기
    kDone,                  // 핸드셰이크 완료
    kFailed,                // 핸드셰이크 실패
};

// ---------------------------------------------------------------------------
// HandshakeAction
//   상태 전이 결과로 relay_handshake()가 수행해야 하는 I/O 동작.
// ---------------------------------------------------------------------------
enum class HandshakeAction : std::uint8_t {
    kRelayToClient,    // 현재 패킷을 클라이언트에 전달
    kRelayToServer,    // 현재 패킷을 서버에 전달
    kComplete,         // 핸드셰이크 완료 — ctx 업데이트 후 종료
    kTerminate,        // 에러 — 세션 종료 (클라이언트에 ERR 먼저 전달 후)
    kTerminateNoRelay, // 에러 — ERR 전달 없이 세션 종료 (unknown packet)
};

// ---------------------------------------------------------------------------
// HandshakeTransition
//   process_handshake_packet()의 반환값.
//   다음 상태와 수행할 액션을 담는다.
// ---------------------------------------------------------------------------
struct HandshakeTransition {
    HandshakeState  next_state{HandshakeState::kFailed};
    HandshakeAction action{HandshakeAction::kTerminate};
};

// ---------------------------------------------------------------------------
// classify_auth_response
//   auth 응답 패킷 payload의 첫 바이트 + 길이로 AuthResponseType을 분류한다.
//   소켓과 무관한 순수 함수.
//
//   payload: auth 응답 패킷의 payload (헤더 4바이트 제외)
//   반환: AuthResponseType
// ---------------------------------------------------------------------------
auto classify_auth_response(std::span<const std::uint8_t> payload) noexcept
    -> AuthResponseType;

// ---------------------------------------------------------------------------
// process_handshake_packet
//   현재 핸드셰이크 상태 + 수신 패킷 payload → 다음 상태 + 액션.
//   소켓과 무관한 순수 함수.
//
//   current_state : 현재 상태
//   payload       : 수신된 패킷의 payload
//   round_trips   : 현재까지의 AuthMoreData/AuthSwitch 라운드트립 횟수
//                   (무한 루프 방지용)
//
//   반환: 성공 시 HandshakeTransition
//         실패 시 std::unexpected(ParseError) — 상태 불일치 등
// ---------------------------------------------------------------------------
auto process_handshake_packet(HandshakeState                current_state,
                              std::span<const std::uint8_t> payload,
                              int                           round_trips) noexcept
    -> std::expected<HandshakeTransition, ParseError>;

// ---------------------------------------------------------------------------
// extract_handshake_response_fields
//   HandshakeResponse41 payload에서 username과 db_name을 추출한다.
//   소켓과 무관한 순수 함수.
//
//   payload  : HandshakeResponse 패킷의 payload
//   out_user : [out] 추출된 username
//   out_db   : [out] 추출된 db_name (CLIENT_CONNECT_WITH_DB 미설정 시 빈 문자열)
//
//   반환: 성공 시 std::expected<void, ParseError>{}
//         실패 시 std::unexpected(ParseError{kMalformedPacket, ...})
// ---------------------------------------------------------------------------
auto extract_handshake_response_fields(
    std::span<const std::uint8_t> payload,
    std::string&                  out_user,
    std::string&                  out_db) noexcept -> std::expected<void, ParseError>;

}  // namespace detail
