#include "proxy/session.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <spdlog/spdlog.h>

#include <array>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <format>
#include <span>
#include <vector>

// ---------------------------------------------------------------------------
// Session — 구현
//
// 흐름:
//   1. SessionContext 초기화
//   2. stats_.on_connection_open()
//   3. TCP connect → (backend SSL이면 TLS 핸드셰이크 후 server_stream_ 재생성)
//   4. HandshakeRelay::relay_handshake()
//   5. state_ = kReady → logger_.log_connection("connect")
//   6. 커맨드 루프:
//        헤더 4B + payload 읽기 → MysqlPacket::parse()
//        COM_QUIT → break
//        COM_QUERY → parse → policy → block/allow 분기
//        기타     → 서버 투명 릴레이
//   7. state_ = kClosed → stats/logger 정리 → 소켓 close
// ---------------------------------------------------------------------------

// 기본 SQL Injection 패턴 (InjectionDetector 초기화)
// NOLINTNEXTLINE(cert-err58-cpp)
static const std::vector<std::string> kDefaultInjectionPatterns = {
    R"(UNION\s+SELECT)",
    R"(('\s*OR\s+['"\d]))",
    R"(SLEEP\s*\()",
    R"(BENCHMARK\s*\()",
    R"(LOAD_FILE\s*\()",
    R"(INTO\s+OUTFILE)",
    R"(INTO\s+DUMPFILE)",
    R"(;\s*(DROP|DELETE|UPDATE|INSERT|ALTER|CREATE))",
    R"(--\s*$)",
    R"(/\*.*\*/)",
};

// ---------------------------------------------------------------------------
// Session 생성자
// ---------------------------------------------------------------------------
Session::Session(std::uint64_t session_id,
                 AsyncStream client_stream,
                 boost::asio::ip::tcp::endpoint server_endpoint,
                 boost::asio::ssl::context* backend_ssl_ctx,
                 bool backend_ssl_verify,
                 const std::string& backend_tls_server_name,  // NOLINT(modernize-pass-by-value)
                 std::shared_ptr<PolicyEngine> policy,
                 std::shared_ptr<StructuredLogger> logger,
                 std::shared_ptr<StatsCollector> stats)
    : session_id_{session_id},
      client_stream_{std::move(client_stream)}
      // server_stream_: 임시 tcp::socket으로 초기화 (run()에서 교체)
      ,
      server_stream_{boost::asio::ip::tcp::socket{client_stream_.get_executor()}},
      server_endpoint_{std::move(server_endpoint)},
      backend_ssl_ctx_{backend_ssl_ctx},
      backend_ssl_verify_{backend_ssl_verify},
      backend_tls_server_name_{backend_tls_server_name},
      policy_{std::move(policy)},
      logger_{std::move(logger)},
      stats_{std::move(stats)},
      ctx_{},
      // NOLINTNEXTLINE(cppcoreguidelines-use-default-member-init,modernize-use-default-member-init)
      state_{SessionState::kHandshaking},
      strand_{boost::asio::make_strand(client_stream_.get_executor())},
      sql_parser_{},
      injection_detector_{kDefaultInjectionPatterns},
      proc_detector_{},
      closing_{false} {}

// ---------------------------------------------------------------------------
// 내부 헬퍼: 패킷 1개 읽기 (4바이트 헤더 + payload)
// ---------------------------------------------------------------------------
namespace {

// ---------------------------------------------------------------------------
// RelayBuffer
//   서버→클라이언트 릴레이 전용 버퍼.
//
//   읽기: async_read_some으로 큰 청크를 읽어 내부 링 버퍼에 누적.
//         PacketView는 다음 read_packet / ensure_available 호출 전까지만 유효.
//         → payload 검사 후 enqueue를 반드시 같은 코루틴 프레임에서 수행할 것.
//
//   쓰기: enqueue()로 원시 바이트를 출력 버퍼에 모아 flush()로 한 번에 전송.
// ---------------------------------------------------------------------------
class RelayBuffer {
public:
    static constexpr std::size_t kInitBufSize = 65536UL;     // 64 KB
    static constexpr std::size_t kFlushThreshold = 65536UL;  // 64 KB

    // PacketView: rbuf_ 내부를 참조하는 제로카피 뷰.
    // ensure_available() 호출(= 다음 read_packet)까지만 유효.
    struct PacketView {
        std::span<const std::uint8_t> raw;      // header(4) + payload
        std::span<const std::uint8_t> payload;  // header 이후
        std::uint8_t sequence_id{};
    };

    explicit RelayBuffer(AsyncStream& stream)
        : read_stream_{&stream}, rbuf_(kInitBufSize), wbuf_{} {
        wbuf_.reserve(kInitBufSize);
    }

    // 서버에서 패킷 1개 읽기 (버퍼에서 제로카피)
    auto read_packet() -> boost::asio::awaitable<std::expected<PacketView, ParseError>> {
        // 헤더 4바이트 확보
        if (!co_await ensure_available(4)) {
            co_return std::unexpected(ParseError{.code = ParseErrorCode::kMalformedPacket,
                                                 .message = "failed to read packet header",
                                                 .context = "eof or read error"});
        }

        // payload 길이 파싱 (3바이트 LE)
        const std::uint32_t payload_len = static_cast<std::uint32_t>(rbuf_[rpos_]) |
                                          (static_cast<std::uint32_t>(rbuf_[rpos_ + 1]) << 8U) |
                                          (static_cast<std::uint32_t>(rbuf_[rpos_ + 2]) << 16U);

        const std::size_t total = 4 + payload_len;

        // 전체 패킷 확보
        if (!co_await ensure_available(total)) {
            co_return std::unexpected(ParseError{.code = ParseErrorCode::kMalformedPacket,
                                                 .message = "failed to read packet payload",
                                                 .context = "eof or read error"});
        }

        // 제로카피 뷰 반환 — 다음 ensure_available 이전까지만 유효
        PacketView view{
            .raw = std::span<const std::uint8_t>(rbuf_.data() + rpos_, total),
            .payload = std::span<const std::uint8_t>(rbuf_.data() + rpos_ + 4, payload_len),
            .sequence_id = rbuf_[rpos_ + 3],
        };
        rpos_ += total;

        co_return view;
    }

    // 원시 바이트를 출력 버퍼에 추가 (복사 1회, 직렬화 불필요)
    void enqueue(std::span<const std::uint8_t> data) {
        wbuf_.insert(wbuf_.end(), data.begin(), data.end());
    }

    // 출력 버퍼가 flush 임계값을 초과하는지 확인
    [[nodiscard]] bool should_flush() const noexcept { return wbuf_.size() >= kFlushThreshold; }

    // 출력 버퍼를 대상 스트림으로 flush
    auto flush(AsyncStream& dest) -> boost::asio::awaitable<std::expected<void, ParseError>> {
        if (wbuf_.empty()) {
            co_return std::expected<void, ParseError>{};
        }

        boost::system::error_code ec;
        co_await boost::asio::async_write(
            dest,
            boost::asio::buffer(wbuf_),
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        wbuf_.clear();

        if (ec) {
            co_return std::unexpected(ParseError{.code = ParseErrorCode::kInternalError,
                                                 .message = "failed to flush relay buffer",
                                                 .context = ec.message()});
        }
        co_return std::expected<void, ParseError>{};
    }

private:
    // 버퍼에 n바이트 이상이 연속으로 확보될 때까지 읽기를 반복한다.
    auto ensure_available(std::size_t n) -> boost::asio::awaitable<bool> {
        while (rend_ - rpos_ < n) {
            // 컴팩션: 소비된 앞쪽 공간을 회수한다.
            if (rpos_ > 0) {
                const std::size_t avail = rend_ - rpos_;
                std::memmove(rbuf_.data(), rbuf_.data() + rpos_, avail);
                rend_ = avail;
                rpos_ = 0;
            }

            // 남은 공간이 부족하면 버퍼 확장
            const std::size_t need = n - (rend_ - rpos_);
            if (rbuf_.size() - rend_ < need) {
                rbuf_.resize(std::max(rbuf_.size() * 2, rend_ + n + 4096));
            }

            boost::system::error_code ec;
            const auto bytes = co_await read_stream_->async_read_some(
                boost::asio::buffer(rbuf_.data() + rend_, rbuf_.size() - rend_),
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));

            if (ec || bytes == 0) {
                co_return false;
            }
            rend_ += bytes;
        }
        co_return true;
    }

    AsyncStream* read_stream_;
    std::vector<std::uint8_t> rbuf_;
    std::size_t rpos_{0};  // 읽기 시작 위치
    std::size_t rend_{0};  // 유효 데이터 끝 위치

    std::vector<std::uint8_t> wbuf_;
};

// ---------------------------------------------------------------------------
// ClientReadBuffer: 클라이언트 스트림에서 패킷을 읽기 위한 버퍼.
// read_one_packet의 2회 async_read(header + payload)를 1회 async_read_some로 줄인다.
// ---------------------------------------------------------------------------
class ClientReadBuffer {
public:
    static constexpr std::size_t kBufSize = 16384UL;  // 16 KB (COM_QUERY 패킷은 보통 < 1KB)

    explicit ClientReadBuffer(AsyncStream& stream) : stream_{&stream}, buf_(kBufSize) {}

    auto read_packet() -> boost::asio::awaitable<std::expected<MysqlPacket, ParseError>> {
        // 헤더 4바이트 확보
        if (!co_await ensure_available(4)) {
            co_return std::unexpected(ParseError{.code = ParseErrorCode::kMalformedPacket,
                                                 .message = "failed to read packet header",
                                                 .context = "eof or read error"});
        }

        const std::uint32_t payload_len = static_cast<std::uint32_t>(buf_[pos_]) |
                                          (static_cast<std::uint32_t>(buf_[pos_ + 1]) << 8U) |
                                          (static_cast<std::uint32_t>(buf_[pos_ + 2]) << 16U);

        const std::size_t total = 4 + payload_len;

        if (!co_await ensure_available(total)) {
            co_return std::unexpected(ParseError{.code = ParseErrorCode::kMalformedPacket,
                                                 .message = "failed to read packet payload",
                                                 .context = "eof or read error"});
        }

        auto result = MysqlPacket::parse(std::span<const std::uint8_t>(buf_.data() + pos_, total));
        pos_ += total;

        co_return result;
    }

private:
    auto ensure_available(std::size_t n) -> boost::asio::awaitable<bool> {
        while (end_ - pos_ < n) {
            // 컴팩션
            if (pos_ > 0) {
                const std::size_t avail = end_ - pos_;
                if (avail > 0) {
                    std::memmove(buf_.data(), buf_.data() + pos_, avail);
                }
                end_ = avail;
                pos_ = 0;
            }
            if (buf_.size() - end_ < n - (end_ - pos_)) {
                buf_.resize(std::max(buf_.size() * 2, end_ + n + 4096));
            }

            boost::system::error_code ec;
            const auto bytes = co_await stream_->async_read_some(
                boost::asio::buffer(buf_.data() + end_, buf_.size() - end_),
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));

            if (ec || bytes == 0) {
                co_return false;
            }
            end_ += bytes;
        }
        co_return true;
    }

    AsyncStream* stream_;
    std::vector<std::uint8_t> buf_;
    std::size_t pos_{0};
    std::size_t end_{0};
};

auto write_packet_raw(AsyncStream& stream, const MysqlPacket& pkt)
    -> boost::asio::awaitable<std::expected<void, ParseError>> {
    // scatter-gather I/O: 헤더를 스택에 구성하고 payload 와 함께 한 번에 전송
    // → pkt.serialize() 의 벡터 할당+복사 제거
    const auto len = pkt.payload_length();
    std::array<std::uint8_t, 4> header{
        static_cast<std::uint8_t>(len & 0xFFU),
        static_cast<std::uint8_t>((len >> 8U) & 0xFFU),
        static_cast<std::uint8_t>((len >> 16U) & 0xFFU),
        pkt.sequence_id(),
    };

    const std::array<boost::asio::const_buffer, 2> bufs{
        boost::asio::buffer(header),
        boost::asio::buffer(pkt.payload().data(), pkt.payload().size()),
    };

    boost::system::error_code ec;
    co_await boost::asio::async_write(
        stream, bufs, boost::asio::redirect_error(boost::asio::use_awaitable, ec));

    if (ec) {
        co_return std::unexpected(ParseError{.code = ParseErrorCode::kInternalError,
                                             .message = "failed to write packet",
                                             .context = ec.message()});
    }

    co_return std::expected<void, ParseError>{};
}

auto parse_lenenc_integer(std::span<const std::uint8_t> payload,
                          std::size_t& offset,
                          std::uint64_t& value) -> bool {
    if (offset >= payload.size()) {
        return false;
    }

    const std::uint8_t first = payload[offset++];
    if (first < 0xFB) {
        value = first;
        return true;
    }

    if (first == 0xFC) {
        if (offset + 2 > payload.size()) {
            return false;
        }
        value = static_cast<std::uint64_t>(payload[offset]) |
                (static_cast<std::uint64_t>(payload[offset + 1]) << 8U);
        offset += 2;
        return true;
    }

    if (first == 0xFD) {
        if (offset + 3 > payload.size()) {
            return false;
        }
        value = static_cast<std::uint64_t>(payload[offset]) |
                (static_cast<std::uint64_t>(payload[offset + 1]) << 8U) |
                (static_cast<std::uint64_t>(payload[offset + 2]) << 16U);
        offset += 3;
        return true;
    }

    if (first == 0xFE) {
        if (offset + 8 > payload.size()) {
            return false;
        }
        value = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(payload[offset + i]) << (8ULL * i);
        }
        offset += 8;
        return true;
    }

    // 0xFB(NULL)는 lenenc integer가 아닌 row cell marker.
    return false;
}

auto consume_lenenc_text_cell(std::span<const std::uint8_t> payload, std::size_t& offset) -> bool {
    if (offset >= payload.size()) {
        return false;
    }

    if (payload[offset] == 0xFB) {  // NULL
        ++offset;
        return true;
    }

    std::uint64_t len = 0;
    if (!parse_lenenc_integer(payload, offset, len)) {
        return false;
    }
    if (offset + len > payload.size()) {
        return false;
    }
    offset += static_cast<std::size_t>(len);
    return true;
}

auto is_text_row_packet(std::span<const std::uint8_t> payload, std::uint8_t column_count) -> bool {
    std::size_t offset = 0;
    for (std::uint8_t i = 0; i < column_count; ++i) {
        if (!consume_lenenc_text_cell(payload, offset)) {
            return false;
        }
    }
    return offset == payload.size();
}

auto is_resultset_final_ok_packet(std::span<const std::uint8_t> payload) -> bool {
    if (payload.empty() || payload[0] != 0x00) {
        return false;
    }

    std::size_t offset = 1;
    std::uint64_t ignored = 0;
    if (!parse_lenenc_integer(payload, offset, ignored)) {
        return false;
    }
    if (!parse_lenenc_integer(payload, offset, ignored)) {
        return false;
    }

    // status_flags(2) + warnings(2) 는 최소 필수 필드
    return offset + 4 <= payload.size();
}

auto is_metadata_terminator_packet(std::span<const std::uint8_t> payload) -> bool {
    return (!payload.empty() && payload[0] == 0xFE && payload.size() < 9) ||
           is_resultset_final_ok_packet(payload);
}

auto relay_stmt_prepare_section(RelayBuffer& relay, std::uint16_t count, std::uint64_t session_id)
    -> boost::asio::awaitable<std::expected<void, ParseError>> {
    for (std::uint16_t i = 0; i < count; ++i) {
        auto def_result = co_await relay.read_packet();
        if (!def_result) {
            co_return std::unexpected(def_result.error());
        }
        relay.enqueue(def_result->raw);
    }

    // terminator packet
    auto term_result = co_await relay.read_packet();
    if (!term_result) {
        co_return std::unexpected(term_result.error());
    }

    // payload 검사는 enqueue 전에, span이 아직 유효한 상태에서 수행
    const auto payload = term_result->payload;
    const bool valid_terminator = is_metadata_terminator_packet(payload);
    const std::uint8_t first_byte_log = payload.empty() ? 0U : payload[0];
    const std::size_t payload_len_log = payload.size();

    relay.enqueue(term_result->raw);

    if (!valid_terminator) {
        spdlog::warn("[session {}] unexpected COM_STMT_PREPARE terminator: 0x{:02x} (len={})",
                     session_id,
                     static_cast<unsigned>(first_byte_log),
                     payload_len_log);
    }

    co_return std::expected<void, ParseError>{};
}

// ---------------------------------------------------------------------------
// relay_server_response_buffered
//   MySQL 서버 응답(OK / ERR / Result Set)이 완료될 때까지 RelayBuffer로 읽어
//   클라이언트에 배치 릴레이한다.
//
//   제로카피 원칙:
//     - 서버에서 읽은 원시 바이트는 MysqlPacket 파싱/직렬화 없이 그대로 전달.
//     - PacketView.payload 참조는 enqueue 전에 필요한 값을 지역 변수로 복사한다
//       (ensure_available이 compact/realloc 시 span이 무효화되므로).
// ---------------------------------------------------------------------------
auto relay_server_response_buffered(RelayBuffer& relay,
                                    AsyncStream& client_stream,
                                    CommandType request_type,
                                    [[maybe_unused]] std::uint8_t request_seq_id,
                                    std::uint64_t session_id)
    -> boost::asio::awaitable<std::expected<void, ParseError>> {
    enum class ResponseState {  // NOLINT(performance-enum-size)
        kFirst,                 // 첫 패킷 분석 중
        kColumnDefs,            // column definition 읽는 중
        kRows,                  // row 데이터 읽는 중
        kDone,                  // 응답 완료
    };

    // --- 첫 패킷 ---
    auto first_result = co_await relay.read_packet();
    if (!first_result) {
        co_return std::unexpected(first_result.error());
    }

    // payload 검사 전에 필요한 값을 지역 변수로 추출 (span 유효 구간 내)
    const std::uint8_t first_seq_id = first_result->sequence_id;
    const bool first_payload_empty = first_result->payload.empty();
    std::uint8_t first_byte = 0;
    std::size_t first_payload_size = 0;
    std::uint16_t num_columns = 0;
    std::uint16_t num_params = 0;

    if (!first_payload_empty) {
        first_byte = first_result->payload[0];
        first_payload_size = first_result->payload.size();

        // COM_STMT_PREPARE OK 파싱 (payload[5..8]) — enqueue 전에 수행
        if (first_byte == 0x00 && request_type == CommandType::kComStmtPrepare &&
            first_payload_size >= 12) {
            num_columns = static_cast<std::uint16_t>(first_result->payload[5]) |
                          (static_cast<std::uint16_t>(first_result->payload[6]) << 8U);
            num_params = static_cast<std::uint16_t>(first_result->payload[7]) |
                         (static_cast<std::uint16_t>(first_result->payload[8]) << 8U);
        }
    }

    relay.enqueue(first_result->raw);  // enqueue 후에는 first_result->payload 참조 금지

    if (first_payload_empty) {
        co_return co_await relay.flush(client_stream);
    }

    // ERR (0xFF) → 즉시 flush
    if (first_byte == 0xFF) {
        co_return co_await relay.flush(client_stream);
    }

    // OK (0x00)
    if (first_byte == 0x00) {
        if (request_type == CommandType::kComStmtPrepare) {
            if (first_payload_size < 12) {
                spdlog::warn("[session {}] short COM_STMT_PREPARE OK payload: {} bytes",
                             session_id,
                             first_payload_size);
                co_return co_await relay.flush(client_stream);
            }

            if (num_params > 0) {
                auto r = co_await relay_stmt_prepare_section(relay, num_params, session_id);
                if (!r) {
                    co_return std::unexpected(r.error());
                }
            }
            if (num_columns > 0) {
                auto r = co_await relay_stmt_prepare_section(relay, num_columns, session_id);
                if (!r) {
                    co_return std::unexpected(r.error());
                }
            }
        }
        co_return co_await relay.flush(client_stream);
    }

    // EOF (0xFE, size < 9) → 즉시 flush (비정상)
    if (first_byte == 0xFE && first_payload_size < 9) {
        co_return co_await relay.flush(client_stream);
    }

    // LOCAL_INFILE (0xFB)
    if (first_byte == 0xFB) {
        spdlog::warn("[session {}] unsupported LOCAL_INFILE response (0xFB) from server",
                     session_id);
        // 이미 enqueue된 패킷을 flush 후 에러 반환
        auto f = co_await relay.flush(client_stream);
        if (!f) {
            co_return std::unexpected(f.error());
        }
        co_return std::unexpected(ParseError{.code = ParseErrorCode::kUnsupportedCommand,
                                             .message = "LOCAL_INFILE response is not supported",
                                             .context = "server response first byte = 0xFB"});
    }

    // Result Set: 첫 바이트 = column count (0x01~0xFC)
    if (first_byte < 0x01 || first_byte > 0xFC) {
        spdlog::warn(
            "[session {}] unexpected first byte in response: 0x{:02x}", session_id, first_byte);
        co_return co_await relay.flush(client_stream);
    }

    const std::uint8_t column_count = first_byte;
    std::uint8_t column_defs_read = 0;
    ResponseState state = ResponseState::kColumnDefs;
    std::uint8_t prev_seq_id = first_seq_id;

    while (state != ResponseState::kDone) {
        auto pkt_result = co_await relay.read_packet();
        if (!pkt_result) {
            co_return std::unexpected(pkt_result.error());
        }

        // PacketView 정보 추출 — enqueue 전에 수행 (span 유효 구간)
        const std::uint8_t pkt_seq_id = pkt_result->sequence_id;
        const bool pkt_payload_empty = pkt_result->payload.empty();
        std::uint8_t byte0 = 0;
        std::size_t pkt_payload_size = 0;
        bool is_row_or_coldef = false;

        if (!pkt_payload_empty) {
            byte0 = pkt_result->payload[0];
            pkt_payload_size = pkt_result->payload.size();
        }

        // kRows 상태의 최종 OK 판별 — payload span 유효 구간 내에서만 가능
        if (!pkt_payload_empty && state == ResponseState::kRows && byte0 == 0x00) {
            is_row_or_coldef = is_text_row_packet(pkt_result->payload, column_count) ||
                               !is_resultset_final_ok_packet(pkt_result->payload);
        }

        relay.enqueue(pkt_result->raw);  // enqueue 후 pkt_result->payload 참조 금지

        // 큰 result set 중간 flush
        if (relay.should_flush()) {
            auto f = co_await relay.flush(client_stream);
            if (!f) {
                co_return std::unexpected(f.error());
            }
        }

        if (pkt_payload_empty) {
            break;
        }

        if (byte0 == 0xFF) {
            state = ResponseState::kDone;
            continue;
        }

        if (pkt_seq_id < prev_seq_id && prev_seq_id != 0xFF) {
            spdlog::warn("[session {}] seq_id reversed ({} -> {}), stopping relay",
                         session_id,
                         prev_seq_id,
                         pkt_seq_id);
            state = ResponseState::kDone;
            continue;
        }
        prev_seq_id = pkt_seq_id;

        switch (state) {
            case ResponseState::kColumnDefs:
                if (byte0 == 0xFE && pkt_payload_size < 9) {
                    state = ResponseState::kRows;
                } else if (byte0 == 0xFF) {
                    state = ResponseState::kDone;
                } else {
                    ++column_defs_read;
                    if (column_defs_read > column_count + 1) {
                        spdlog::warn("[session {}] too many column definitions: {} > {}",
                                     session_id,
                                     column_defs_read,
                                     column_count);
                        state = ResponseState::kDone;
                    }
                }
                break;

            case ResponseState::kRows: {
                const bool eof_or_err = (byte0 == 0xFE && pkt_payload_size < 9) || byte0 == 0xFF;
                // is_row_or_coldef: true이면 일반 row/coldef → 아직 결과 진행 중
                const bool final_ok =
                    request_type == CommandType::kComQuery && byte0 == 0x00 && !is_row_or_coldef;
                if (eof_or_err || final_ok) {
                    state = ResponseState::kDone;
                }
                break;
            }

            case ResponseState::kDone:
            case ResponseState::kFirst:
                break;
        }
    }

    co_return co_await relay.flush(client_stream);
}

}  // namespace

// ---------------------------------------------------------------------------
// Session::run
// ---------------------------------------------------------------------------
auto Session::run() -> boost::asio::awaitable<void> {
    // -----------------------------------------------------------------------
    // 1. SessionContext 초기화
    // -----------------------------------------------------------------------
    ctx_.session_id = session_id_;
    ctx_.connected_at = std::chrono::system_clock::now();

    // 클라이언트 IP/포트 추출
    boost::system::error_code peer_ec;
    const auto remote_ep = client_stream_.lowest_layer().remote_endpoint(peer_ec);
    if (!peer_ec) {
        ctx_.client_ip = remote_ep.address().to_string();
        ctx_.client_port = remote_ep.port();
    }

    // -----------------------------------------------------------------------
    // 2. stats: 연결 열기
    // -----------------------------------------------------------------------
    stats_->on_connection_open();

    struct StatsGuard {  // NOLINT(cppcoreguidelines-special-member-functions)
        StatsCollector* stats;
        ~StatsGuard() { stats->on_connection_close(); }
    } const stats_guard{stats_.get()};

    // -----------------------------------------------------------------------
    // 3. Frontend TLS 핸드셰이크 (클라이언트 구간 TLS 활성화 시)
    // -----------------------------------------------------------------------
    if (client_stream_.is_ssl()) {
        boost::system::error_code tls_ec;
        co_await client_stream_.async_handshake(
            boost::asio::ssl::stream_base::server,
            boost::asio::redirect_error(boost::asio::use_awaitable, tls_ec));

        if (tls_ec) {
            spdlog::warn(
                "[session {}] frontend TLS handshake failed: {}", session_id_, tls_ec.message());
            state_ = SessionState::kClosed;
            boost::system::error_code close_ec;
            // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
            client_stream_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both,
                                                   close_ec);
            // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
            client_stream_.lowest_layer().close(close_ec);
            co_return;
        }
    }

    // -----------------------------------------------------------------------
    // 4. MySQL 서버 TCP connect
    // -----------------------------------------------------------------------
    // 로컬 tcp::socket으로 서버에 먼저 연결
    boost::asio::ip::tcp::socket raw_server_sock{client_stream_.get_executor()};
    boost::system::error_code connect_ec;
    co_await boost::asio::async_connect(
        raw_server_sock,
        std::vector{server_endpoint_},
        boost::asio::redirect_error(boost::asio::use_awaitable, connect_ec));

    if (connect_ec) {
        spdlog::error(
            "[session {}] upstream connect failed: {}", session_id_, connect_ec.message());

        const auto err_pkt = MysqlPacket::make_error(
            2003, std::format("Can't connect to MySQL server ({})", connect_ec.message()), 0);
        boost::system::error_code wr_ec;
        const auto err_bytes = err_pkt.serialize();
        co_await boost::asio::async_write(
            client_stream_,
            boost::asio::buffer(err_bytes),
            boost::asio::redirect_error(boost::asio::use_awaitable, wr_ec));

        state_ = SessionState::kClosed;
        boost::system::error_code close_ec;
        // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
        client_stream_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both,
                                               close_ec);
        // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
        client_stream_.lowest_layer().close(close_ec);
        co_return;
    }

    // TCP_NODELAY: Nagle 알고리즘 비활성화 (업스트림 서버 소켓)
    {
        boost::system::error_code nodelay_ec;
        // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
        raw_server_sock.set_option(boost::asio::ip::tcp::no_delay(true), nodelay_ec);
        if (nodelay_ec) {
            spdlog::warn("[session {}] failed to set TCP_NODELAY on server socket: {}",
                         session_id_,
                         nodelay_ec.message());
        }
    }

    // -----------------------------------------------------------------------
    // 5. Backend SSL 핸드셰이크 (backend_ssl_ctx_가 유효한 경우)
    //    TCP connect 성공 후 ssl::stream으로 업그레이드하고 server_stream_ 교체
    // -----------------------------------------------------------------------
    if (backend_ssl_ctx_ != nullptr) {
        spdlog::debug("[session {}] backend SSL: upgrading TCP to TLS", session_id_);

        // ssl::stream으로 래핑
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket> ssl_server_sock{
            std::move(raw_server_sock), *backend_ssl_ctx_};

        const auto verify_name = backend_tls_server_name_.empty()
                                     ? server_endpoint_.address().to_string()
                                     : backend_tls_server_name_;
        boost::system::error_code ip_ec;
        const bool verify_name_is_ip =
            !boost::asio::ip::make_address(verify_name, ip_ec).is_unspecified() && !ip_ec;

        // SNI는 호스트명 기반 TLS에서만 설정한다.
        if (!verify_name.empty() && !verify_name_is_ip) {
            if (SSL_set_tlsext_host_name(ssl_server_sock.native_handle(), verify_name.c_str()) !=
                1) {
                const auto err = ERR_get_error();
                spdlog::error("[session {}] backend TLS SNI setup failed for {}: {}",
                              session_id_,
                              verify_name,
                              err != 0 ? ERR_error_string(err, nullptr) : "unknown");
                state_ = SessionState::kClosed;
                boost::system::error_code close_ec;
                // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
                client_stream_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both,
                                                       close_ec);
                // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
                client_stream_.lowest_layer().close(close_ec);
                co_return;
            }
        }

        // verify_peer가 활성화되면 인증서 체인 + 호스트명/IP 일치 검증을 모두 수행한다.
        if (backend_ssl_verify_) {
            int verify_ok = 0;
            if (verify_name_is_ip) {
                X509_VERIFY_PARAM* verify_param = SSL_get0_param(ssl_server_sock.native_handle());
                verify_ok = X509_VERIFY_PARAM_set1_ip_asc(verify_param, verify_name.c_str());
            } else {
                verify_ok = SSL_set1_host(ssl_server_sock.native_handle(), verify_name.c_str());
            }
            if (verify_ok != 1) {
                const auto err = ERR_get_error();
                spdlog::error(
                    "[session {}] backend TLS hostname verification setup failed for {}: {}",
                    session_id_,
                    verify_name,
                    err != 0 ? ERR_error_string(err, nullptr) : "unknown");
                state_ = SessionState::kClosed;
                boost::system::error_code close_ec;
                // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
                client_stream_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both,
                                                       close_ec);
                // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
                client_stream_.lowest_layer().close(close_ec);
                co_return;
            }
        }

        // TLS 핸드셰이크 (client 역할 — 프록시가 MySQL 서버에 연결하는 클라이언트)
        boost::system::error_code tls_ec;
        co_await ssl_server_sock.async_handshake(
            boost::asio::ssl::stream_base::client,
            boost::asio::redirect_error(boost::asio::use_awaitable, tls_ec));

        if (tls_ec) {
            spdlog::error(
                "[session {}] backend TLS handshake failed: {}", session_id_, tls_ec.message());

            const auto err_pkt =
                MysqlPacket::make_error(2026,  // CR_SSL_CONNECTION_ERROR
                                        std::format("SSL connection error: {}", tls_ec.message()),
                                        0);
            boost::system::error_code wr_ec;
            const auto err_bytes = err_pkt.serialize();
            co_await boost::asio::async_write(
                client_stream_,
                boost::asio::buffer(err_bytes),
                boost::asio::redirect_error(boost::asio::use_awaitable, wr_ec));

            state_ = SessionState::kClosed;
            boost::system::error_code close_ec;
            // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
            client_stream_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both,
                                                   close_ec);
            // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
            client_stream_.lowest_layer().close(close_ec);
            co_return;
        }

        spdlog::debug("[session {}] backend TLS handshake succeeded", session_id_);

        // server_stream_을 TLS stream으로 교체 (move-assign)
        server_stream_ = AsyncStream{std::move(ssl_server_sock)};

    } else {
        // 평문 모드: raw tcp::socket으로 AsyncStream 생성
        server_stream_ = AsyncStream{std::move(raw_server_sock)};
    }

    // -----------------------------------------------------------------------
    // 6. HandshakeRelay::relay_handshake()
    // -----------------------------------------------------------------------
    auto hs_result = co_await HandshakeRelay::relay_handshake(client_stream_, server_stream_, ctx_);

    if (!hs_result) {
        spdlog::error("[session {}] handshake failed: {}", session_id_, hs_result.error().message);
        state_ = SessionState::kClosed;
        boost::system::error_code close_ec;
        // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
        client_stream_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both,
                                               close_ec);
        // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
        client_stream_.lowest_layer().close(close_ec);
        // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
        server_stream_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both,
                                               close_ec);
        // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
        server_stream_.lowest_layer().close(close_ec);
        co_return;
    }

    // -----------------------------------------------------------------------
    // 7. 핸드셰이크 완료 → kReady
    // -----------------------------------------------------------------------
    state_ = SessionState::kReady;

    logger_->log_connection(ConnectionLog{
        .session_id = session_id_,
        .event = "connect",
        .client_ip = ctx_.client_ip,
        .client_port = ctx_.client_port,
        .db_user = ctx_.db_user,
        .timestamp = std::chrono::system_clock::now(),
    });

    spdlog::info(
        "[session {}] handshake done, user={}, db={}", session_id_, ctx_.db_user, ctx_.db_name);

    // -----------------------------------------------------------------------
    // 8. 커맨드 루프
    //    RelayBuffer는 server_stream_ 위에서 서버 응답을 배치로 읽어 클라이언트에 전달.
    //    세션 전체 수명 동안 재사용하여 재할당을 최소화한다.
    // -----------------------------------------------------------------------
    RelayBuffer server_relay(server_stream_);
    ClientReadBuffer client_reader(client_stream_);

    while (true) {
        if (closing_.load(std::memory_order_acquire)) {
            break;
        }

        auto pkt_result = co_await client_reader.read_packet();

        if (!pkt_result) {
            const auto& err = pkt_result.error();
            if (err.context.find("eof") != std::string::npos ||
                err.context.find("End of file") != std::string::npos ||
                err.message.find("header") != std::string::npos) {
                spdlog::debug("[session {}] client disconnected", session_id_);
            } else {
                spdlog::warn("[session {}] read error: {} (context: {})",
                             session_id_,
                             err.message,
                             err.context);
            }
            break;
        }

        const MysqlPacket& pkt = *pkt_result;

        auto cmd_result = extract_command(pkt);
        if (!cmd_result) {
            spdlog::warn("[session {}] malformed command packet: {}",
                         session_id_,
                         cmd_result.error().message);
            break;
        }

        const CommandPacket& cmd = *cmd_result;

        // ---------------------------------------------------------------
        // COM_QUIT
        // ---------------------------------------------------------------
        if (cmd.command_type == CommandType::kComQuit) {
            spdlog::debug("[session {}] COM_QUIT received", session_id_);
            auto fwd = co_await write_packet_raw(server_stream_, pkt);
            (void)fwd;  // QUIT 실패해도 세션 종료
            break;
        }

        // ---------------------------------------------------------------
        // COM_QUERY
        // ---------------------------------------------------------------
        if (cmd.command_type == CommandType::kComQuery) {
            state_ = SessionState::kProcessingQuery;

            const auto query_start = std::chrono::steady_clock::now();

            auto parse_result = sql_parser_.parse(cmd.query);

            PolicyResult policy_result;

            if (!parse_result) {
                spdlog::warn("[session {}] SQL parse error: {} sql={}",
                             session_id_,
                             parse_result.error().message,
                             cmd.query.size() > 200 ? cmd.query.substr(0, 200) + "..." : cmd.query);
                policy_result = policy_->evaluate_error(parse_result.error(), ctx_);
            } else {
                const ParsedQuery& parsed = *parse_result;

                // NOTE: injection_detector_.check() 및 proc_detector_.detect() 결과는
                // 현재 정책 엔진에서 사용하지 않으므로 호출을 제거하여 오버헤드 절감.
                // 향후 정책에 통합 시 여기서 재활성화한다.

                policy_result = policy_->evaluate(parsed, ctx_);
            }

            const auto query_end = std::chrono::steady_clock::now();
            const auto duration =
                std::chrono::duration_cast<std::chrono::microseconds>(query_end - query_start);

            if (policy_result.action == PolicyAction::kBlock) {
                const auto err_pkt =
                    MysqlPacket::make_error(1045,
                                            "Access denied by policy",
                                            static_cast<std::uint8_t>(cmd.sequence_id + 1));

                boost::system::error_code wr_ec;
                const auto err_bytes = err_pkt.serialize();
                co_await boost::asio::async_write(
                    client_stream_,
                    boost::asio::buffer(err_bytes),
                    boost::asio::redirect_error(boost::asio::use_awaitable, wr_ec));

                logger_->log_block(BlockLog{
                    .session_id = session_id_,
                    .db_user = ctx_.db_user,
                    .client_ip = ctx_.client_ip,
                    .raw_sql = cmd.query,
                    .matched_rule = policy_result.matched_rule,
                    .reason = policy_result.reason,
                    .timestamp = std::chrono::system_clock::now(),
                });

                stats_->on_query(true);
                state_ = SessionState::kReady;
                continue;
            }

            // monitor 모드: 차단되었을 쿼리를 로그로만 기록하고 실제로는 통과시킨다.
            // policy_result.action == kLog && monitor_mode == true 인 경우.
            if (policy_result.monitor_mode) {
                logger_->log_block(BlockLog{
                    .session_id = session_id_,
                    .db_user = ctx_.db_user,
                    .client_ip = ctx_.client_ip,
                    .raw_sql = cmd.query,
                    .matched_rule = policy_result.matched_rule,
                    .reason = policy_result.reason,
                    .timestamp = std::chrono::system_clock::now(),
                    .would_block = true,
                });
                stats_->on_monitored_block();
            }

            {
                auto fwd = co_await write_packet_raw(server_stream_, pkt);
                if (!fwd) {
                    spdlog::error("[session {}] failed to forward query to server: {}",
                                  session_id_,
                                  fwd.error().message);
                    break;
                }

                auto relay_result = co_await relay_server_response_buffered(
                    server_relay, client_stream_, cmd.command_type, cmd.sequence_id, session_id_);
                if (!relay_result) {
                    spdlog::warn("[session {}] relay_server_response failed: {}",
                                 session_id_,
                                 relay_result.error().message);
                    break;
                }

                std::uint8_t command_raw = 0;
                std::vector<std::string> tables;
                if (parse_result) {
                    command_raw = static_cast<std::uint8_t>(parse_result->command);
                    tables = parse_result->tables;
                }

                logger_->log_query(QueryLog{
                    .session_id = session_id_,
                    .db_user = ctx_.db_user,
                    .client_ip = ctx_.client_ip,
                    .raw_sql = cmd.query,
                    .command_raw = command_raw,
                    .tables = std::move(tables),
                    .action_raw = static_cast<std::uint8_t>(policy_result.action),
                    .timestamp = std::chrono::system_clock::now(),
                    .duration = duration,
                });

                stats_->on_query(false);
            }

            state_ = SessionState::kReady;
            continue;
        }

        // ---------------------------------------------------------------
        // Prepared Statement 계열 (보안 fail-close)
        // ---------------------------------------------------------------
        if (cmd.command_type == CommandType::kComStmtPrepare ||
            cmd.command_type == CommandType::kComStmtExecute ||
            cmd.command_type == CommandType::kComStmtReset) {
            spdlog::warn("[session {}] blocking unsupported prepared-statement command: 0x{:02x}",
                         session_id_,
                         static_cast<std::uint8_t>(cmd.command_type));

            const auto err_pkt = MysqlPacket::make_error(
                1235,
                "Prepared statements are not supported by proxy policy enforcement",
                static_cast<std::uint8_t>(cmd.sequence_id + 1));

            boost::system::error_code wr_ec;
            const auto err_bytes = err_pkt.serialize();
            co_await boost::asio::async_write(
                client_stream_,
                boost::asio::buffer(err_bytes),
                boost::asio::redirect_error(boost::asio::use_awaitable, wr_ec));

            if (wr_ec) {
                spdlog::warn("[session {}] failed to send prepared-statement rejection: {}",
                             session_id_,
                             wr_ec.message());
                break;
            }

            continue;
        }

        // ---------------------------------------------------------------
        // 기타 커맨드: 서버로 투명 릴레이 + 응답 클라이언트에 릴레이
        // ---------------------------------------------------------------
        {
            auto fwd = co_await write_packet_raw(server_stream_, pkt);
            if (!fwd) {
                spdlog::warn("[session {}] failed to forward command to server: {}",
                             session_id_,
                             fwd.error().message);
                break;
            }

            auto relay_result = co_await relay_server_response_buffered(
                server_relay, client_stream_, cmd.command_type, cmd.sequence_id, session_id_);
            if (!relay_result) {
                spdlog::warn("[session {}] failed to relay server response: {}",
                             session_id_,
                             relay_result.error().message);
                break;
            }
        }
    }

    // -----------------------------------------------------------------------
    // 9. 세션 정리
    // -----------------------------------------------------------------------
    state_ = SessionState::kClosed;

    logger_->log_connection(ConnectionLog{
        .session_id = session_id_,
        .event = "disconnect",
        .client_ip = ctx_.client_ip,
        .client_port = ctx_.client_port,
        .db_user = ctx_.db_user,
        .timestamp = std::chrono::system_clock::now(),
    });

    spdlog::info("[session {}] closed", session_id_);

    boost::system::error_code close_ec;
    // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
    client_stream_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, close_ec);
    // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
    client_stream_.lowest_layer().close(close_ec);
    // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
    server_stream_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, close_ec);
    // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
    server_stream_.lowest_layer().close(close_ec);

    // stats_guard 소멸 시 on_connection_close() 호출됨
}

// ---------------------------------------------------------------------------
// Session::close
// ---------------------------------------------------------------------------
void Session::close() {
    bool expected = false;
    if (closing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        spdlog::debug("[session {}] close() called", session_id_);
        auto self = shared_from_this();
        boost::asio::post(strand_, [self]() {
            boost::system::error_code ec;
            // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
            self->client_stream_.lowest_layer().cancel(ec);
            // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
            self->server_stream_.lowest_layer().cancel(ec);
        });
    }
}

// ---------------------------------------------------------------------------
// Session::state / context
// ---------------------------------------------------------------------------
auto Session::state() const noexcept -> SessionState {
    return state_;
}

auto Session::context() const noexcept -> const SessionContext& {
    return ctx_;
}
