// ---------------------------------------------------------------------------
// test_ssl_tls.cpp
//
// SSL/TLS 설정 및 컨텍스트 초기화 단위 테스트 (DON-31 Phase 3)
//
// 검증 대상:
//   A. ProxyConfig SSL 평탄 필드 기본값
//   C. SSL Context 생성/설정 검증 (init_ssl 간접 테스트)
//   D. Self-signed cert를 사용한 ssl::context 로드 검증
//   E. SSL 비활성화 시 기존 동작 하위 호환
//   F. 실패 케이스 (fail-close 검증)
//   G. Session SSL 모드 테스트
//
// init_ssl()은 ProxyServer private 메서드이므로 직접 호출할 수 없다.
// boost::asio::ssl::context API를 통해 동일한 로직을 재현하여 검증한다.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>

#include "common/async_stream.hpp"
#include "logger/structured_logger.hpp"
#include "policy/policy_engine.hpp"
#include "proxy/proxy_server.hpp"
#include "proxy/session.hpp"
#include "stats/stats_collector.hpp"

// ---------------------------------------------------------------------------
// 픽스처 헬퍼
// ---------------------------------------------------------------------------
namespace {

struct TestCertificateFiles {
    std::filesystem::path
        cert_path;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
    std::filesystem::path
        key_path;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
};

std::shared_ptr<PolicyEngine> make_policy_engine() {
    return std::make_shared<PolicyEngine>(nullptr);
}

std::shared_ptr<StructuredLogger> make_logger() {
    const auto tmp_path = std::filesystem::temp_directory_path() / "test_ssl_tls_logger.log";
    return std::make_shared<StructuredLogger>(LogLevel::kDebug, tmp_path);
}

std::shared_ptr<StatsCollector> make_stats() {
    return std::make_shared<StatsCollector>();
}

// 임시 파일에 PEM 문자열을 쓰고 경로를 반환한다.
std::filesystem::path write_tmp_pem(const std::string& filename, std::string_view pem) {
    const auto path = std::filesystem::temp_directory_path() / filename;
    std::ofstream ofs{path};
    ofs << pem;
    ofs.close();
    return path;
}

std::string shell_quote(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('\'');
    for (const char ch : value) {
        if (ch == '\'') {
            out.append("'\\''");
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('\'');
    return out;
}

std::string make_test_file_basename() {
    static std::mt19937_64 rng{std::random_device{}()};
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return "test_ssl_tls_" + std::to_string(now) + "_" + std::to_string(rng());
}

std::optional<TestCertificateFiles> try_generate_test_certificate_files() {
    const auto base = make_test_file_basename();
    TestCertificateFiles files{
        .cert_path = std::filesystem::temp_directory_path() / (base + "_cert.pem"),
        .key_path = std::filesystem::temp_directory_path() / (base + "_key.pem"),
    };

    const std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " +
                            shell_quote(files.key_path.string()) + " -out " +
                            shell_quote(files.cert_path.string()) +
                            " -days 1 -nodes -subj '/CN=dbgate-test' >/dev/null 2>&1";

    const int rc = std::system(cmd.c_str());  // NOLINT(cert-env33-c,concurrency-mt-unsafe)
    if (rc != 0) {
        return std::nullopt;
    }

    if (!std::filesystem::exists(files.cert_path) || !std::filesystem::exists(files.key_path)) {
        return std::nullopt;
    }

    return files;
}

const std::optional<TestCertificateFiles>& test_certificate_files() {
    static const std::optional<TestCertificateFiles> files = try_generate_test_certificate_files();
    return files;
}

}  // namespace

// ===========================================================================
// A. ProxyConfig SSL 기본값 테스트 (평탄 필드)
// ===========================================================================

// ---------------------------------------------------------------------------
// PA-1. ProxyConfig 기본 생성 시 frontend_ssl_enabled=false
// ---------------------------------------------------------------------------
TEST(ProxyConfigSslTest, DefaultFrontendSslEnabled_IsFalse) {
    const ProxyConfig cfg;
    EXPECT_FALSE(cfg.frontend_ssl_enabled);
}

// ---------------------------------------------------------------------------
// PA-2. ProxyConfig 기본 생성 시 backend_ssl_enabled=false
// ---------------------------------------------------------------------------
TEST(ProxyConfigSslTest, DefaultBackendSslEnabled_IsFalse) {
    const ProxyConfig cfg;
    EXPECT_FALSE(cfg.backend_ssl_enabled);
}

// ---------------------------------------------------------------------------
// PA-3. ProxyConfig 기본 생성 시 frontend_ssl_cert_path/key_path 빈 문자열
// ---------------------------------------------------------------------------
TEST(ProxyConfigSslTest, DefaultFrontendSslPaths_AreEmpty) {
    const ProxyConfig cfg;
    EXPECT_TRUE(cfg.frontend_ssl_cert_path.empty());
    EXPECT_TRUE(cfg.frontend_ssl_key_path.empty());
}

// ---------------------------------------------------------------------------
// PA-4. ProxyConfig 기본 생성 시 backend_ssl_ca_path 빈 문자열
// ---------------------------------------------------------------------------
TEST(ProxyConfigSslTest, DefaultBackendSslCaPath_IsEmpty) {
    const ProxyConfig cfg;
    EXPECT_TRUE(cfg.backend_ssl_ca_path.empty());
}

// ---------------------------------------------------------------------------
// PA-5. ProxyConfig 기본 생성 시 backend_ssl_verify=true
// ---------------------------------------------------------------------------
TEST(ProxyConfigSslTest, DefaultBackendSslVerify_IsTrue) {
    const ProxyConfig cfg;
    EXPECT_TRUE(cfg.backend_ssl_verify);
}

// ---------------------------------------------------------------------------
// PA-6. ProxyConfig 기본 생성 시 upstream_ssl_sni 빈 문자열
// ---------------------------------------------------------------------------
TEST(ProxyConfigSslTest, DefaultUpstreamSslSni_IsEmpty) {
    const ProxyConfig cfg;
    EXPECT_TRUE(cfg.upstream_ssl_sni.empty());
}

// ===========================================================================
// C. SSL Context 생성/설정 검증 (ProxyServer::init_ssl 간접 테스트)
// ===========================================================================

// ---------------------------------------------------------------------------
// SC-1. SSL 비활성화 시 ProxyServer 생성 성공 (기존 동작 하위 호환)
// ---------------------------------------------------------------------------
TEST(ProxyServerSslTest, SslDisabled_ConstructionSucceeds) {
    ProxyConfig cfg;
    cfg.listen_address = "127.0.0.1";
    cfg.listen_port = 23306;
    cfg.upstream_address = "127.0.0.1";
    cfg.upstream_port = 3306;
    cfg.log_level = "info";
    cfg.log_path = "/tmp/test_ssl_proxy.log";
    cfg.uds_socket_path = "/tmp/test_ssl_proxy.sock";
    cfg.policy_path = "/tmp/nonexistent_ssl_policy.yaml";
    // frontend_ssl_enabled = false (기본값)
    // backend_ssl_enabled = false (기본값)

    EXPECT_NO_THROW({ const ProxyServer server{cfg}; });
}

// ---------------------------------------------------------------------------
// SC-2. Frontend SSL 활성화 + 유효 인증서 경로 설정 → ProxyServer 생성 성공
//   (실제 cert 로딩은 run() 내 init_ssl()에서 수행하므로 생성자는 성공)
// ---------------------------------------------------------------------------
TEST(ProxyServerSslTest, FrontendSslEnabled_ConstructionSucceeds) {
    const auto& files = test_certificate_files();
    if (!files.has_value()) {
        GTEST_SKIP() << "openssl 인증서 생성 실패로 테스트를 건너뜀";
    }

    ProxyConfig cfg;
    cfg.listen_address = "127.0.0.1";
    cfg.listen_port = 23307;
    cfg.upstream_address = "127.0.0.1";
    cfg.upstream_port = 3306;
    cfg.log_level = "info";
    cfg.log_path = "/tmp/test_ssl_frontend.log";
    cfg.uds_socket_path = "/tmp/test_ssl_frontend.sock";
    cfg.policy_path = "/tmp/nonexistent_ssl_policy.yaml";

    cfg.frontend_ssl_enabled = true;
    cfg.frontend_ssl_cert_path = files->cert_path.string();
    cfg.frontend_ssl_key_path = files->key_path.string();

    // 생성자에서는 init_ssl()을 호출하지 않으므로 성공해야 함
    EXPECT_NO_THROW({ const ProxyServer server{cfg}; });
}

// ---------------------------------------------------------------------------
// SC-3. Backend SSL 활성화 + 유효 CA 경로 설정 → ProxyServer 생성 성공
// ---------------------------------------------------------------------------
TEST(ProxyServerSslTest, BackendSslEnabled_ConstructionSucceeds) {
    const auto& files = test_certificate_files();
    if (!files.has_value()) {
        GTEST_SKIP() << "openssl 인증서 생성 실패로 테스트를 건너뜀";
    }

    ProxyConfig cfg;
    cfg.listen_address = "127.0.0.1";
    cfg.listen_port = 23308;
    cfg.upstream_address = "127.0.0.1";
    cfg.upstream_port = 3306;
    cfg.log_level = "info";
    cfg.log_path = "/tmp/test_ssl_backend.log";
    cfg.uds_socket_path = "/tmp/test_ssl_backend.sock";
    cfg.policy_path = "/tmp/nonexistent_ssl_policy.yaml";

    cfg.backend_ssl_enabled = true;
    cfg.backend_ssl_ca_path = files->cert_path.string();
    cfg.backend_ssl_verify = true;

    EXPECT_NO_THROW({ const ProxyServer server{cfg}; });
}

// ===========================================================================
// D. Self-signed cert 생성 및 SSL Context 로드 테스트
//    (boost::asio::ssl::context API 직접 사용 — init_ssl 로직 재현)
// ===========================================================================

// ---------------------------------------------------------------------------
// SD-1. 유효한 cert + key → ssl::context 로드 성공
// ---------------------------------------------------------------------------
TEST(SslContextTest, ValidCertAndKey_LoadSucceeds) {
    const auto& files = test_certificate_files();
    if (!files.has_value()) {
        GTEST_SKIP() << "openssl 인증서 생성 실패로 테스트를 건너뜀";
    }

    boost::asio::ssl::context ctx{boost::asio::ssl::context::tls_server};

    boost::system::error_code ec;
    // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
    ctx.use_certificate_chain_file(files->cert_path.string(), ec);
    EXPECT_FALSE(ec) << "cert 로드 실패: " << ec.message();

    // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
    ctx.use_private_key_file(files->key_path.string(), boost::asio::ssl::context::pem, ec);
    EXPECT_FALSE(ec) << "key 로드 실패: " << ec.message();
}

// ---------------------------------------------------------------------------
// SD-2. 유효한 CA cert → ssl::context CA 로드 성공
// ---------------------------------------------------------------------------
TEST(SslContextTest, ValidCaCert_LoadVerifyFileSucceeds) {
    const auto& files = test_certificate_files();
    if (!files.has_value()) {
        GTEST_SKIP() << "openssl 인증서 생성 실패로 테스트를 건너뜀";
    }

    boost::asio::ssl::context ctx{boost::asio::ssl::context::tls_client};

    boost::system::error_code ec;
    // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
    ctx.load_verify_file(files->cert_path.string(), ec);
    EXPECT_FALSE(ec) << "CA 로드 실패: " << ec.message();
}

// ---------------------------------------------------------------------------
// SD-3. TLS 최소 버전(1.2 이상) 옵션 설정 → 에러 없음
//   (no_tlsv1, no_tlsv1_1 옵션 설정 검증 — init_ssl 동일 로직)
// ---------------------------------------------------------------------------
TEST(SslContextTest, Tls12MinVersion_SetOptionsSucceeds) {
    boost::asio::ssl::context ctx{boost::asio::ssl::context::tls_server};

    // init_ssl()에서 사용하는 동일 옵션 세트
    EXPECT_NO_THROW({
        ctx.set_options(
            boost::asio::ssl::context::default_workarounds | boost::asio::ssl::context::no_sslv2 |
            boost::asio::ssl::context::no_sslv3 | boost::asio::ssl::context::no_tlsv1 |
            boost::asio::ssl::context::no_tlsv1_1 | boost::asio::ssl::context::single_dh_use);
    });
}

// ---------------------------------------------------------------------------
// SD-4. ssl::stream 생성 (평문 소켓 + ssl::context) → 생성 성공
// ---------------------------------------------------------------------------
TEST(SslContextTest, SslStream_ConstructionSucceeds) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx{boost::asio::ssl::context::tls_client};

    AsyncStream::ssl_socket ssl_sock{boost::asio::ip::tcp::socket{ioc}, ssl_ctx};
    const AsyncStream stream{std::move(ssl_sock)};
    EXPECT_TRUE(stream.is_ssl());
}

// ---------------------------------------------------------------------------
// SD-5. verify_peer=true 시 verify_peer 모드 설정 → 에러 없음
// ---------------------------------------------------------------------------
TEST(SslContextTest, VerifyPeerMode_SetVerifyModeSucceeds) {
    boost::asio::ssl::context ctx{boost::asio::ssl::context::tls_client};

    EXPECT_NO_THROW({ ctx.set_verify_mode(boost::asio::ssl::verify_peer); });
}

// ---------------------------------------------------------------------------
// SD-6. verify_peer=false 시 verify_none 모드 설정 → 에러 없음
// ---------------------------------------------------------------------------
TEST(SslContextTest, VerifyNoneMode_SetVerifyModeSucceeds) {
    boost::asio::ssl::context ctx{boost::asio::ssl::context::tls_client};

    EXPECT_NO_THROW({ ctx.set_verify_mode(boost::asio::ssl::verify_none); });
}

// ===========================================================================
// E. SSL 비활성화 시 기존 동작 하위 호환 테스트
// ===========================================================================

// ---------------------------------------------------------------------------
// SE-1. SSL 비활성화 상태에서 Session 생성 (backend_ssl_ctx=nullptr) → 평문 모드
// ---------------------------------------------------------------------------
TEST(SessionSslTest, BackendSslCtxNull_PlaintextMode) {
    boost::asio::io_context ioc;
    AsyncStream client_stream{boost::asio::ip::tcp::socket{ioc}};

    const auto server_ep =
        boost::asio::ip::tcp::endpoint{boost::asio::ip::address_v4::loopback(), 3306};

    const auto session = std::make_shared<Session>(10ULL,
                                                   std::move(client_stream),
                                                   server_ep,
                                                   nullptr,  // backend_ssl_ctx=nullptr: 평문 모드
                                                   false,
                                                   "",
                                                   make_policy_engine(),
                                                   make_logger(),
                                                   make_stats());

    // 세션이 정상 생성되어야 함
    EXPECT_EQ(session->state(), SessionState::kHandshaking);
}

// ---------------------------------------------------------------------------
// SE-2. SSL 비활성화 상태에서 AsyncStream은 is_ssl()=false
// ---------------------------------------------------------------------------
TEST(SessionSslTest, PlaintextAsyncStream_IsNotSsl) {
    boost::asio::io_context ioc;
    const AsyncStream stream{boost::asio::ip::tcp::socket{ioc}};

    EXPECT_FALSE(stream.is_ssl());
}

// ---------------------------------------------------------------------------
// SE-3. SSL 비활성화 ProxyServer 생성 후 SSL 관련 필드 기본값 확인
// ---------------------------------------------------------------------------
TEST(ProxyServerSslTest, SslDisabled_DefaultSslFieldsUnchanged) {
    ProxyConfig cfg;
    cfg.listen_address = "127.0.0.1";
    cfg.listen_port = 23309;
    cfg.upstream_address = "127.0.0.1";
    cfg.upstream_port = 3306;
    cfg.log_level = "info";
    cfg.log_path = "/tmp/test_ssl_disabled.log";
    cfg.uds_socket_path = "/tmp/test_ssl_disabled.sock";
    cfg.policy_path = "/tmp/nonexistent_ssl_policy.yaml";

    // SSL 비활성화 상태에서 생성 성공 확인
    EXPECT_NO_THROW({ const ProxyServer server{cfg}; });

    // 설정 기본값 확인 (변경되지 않아야 함)
    EXPECT_FALSE(cfg.frontend_ssl_enabled);
    EXPECT_FALSE(cfg.backend_ssl_enabled);
}

// ===========================================================================
// F. 실패 케이스 (fail-close 검증)
//    boost::asio::ssl::context API 직접 사용으로 에러 경로 검증
// ===========================================================================

// ---------------------------------------------------------------------------
// SF-1. 존재하지 않는 cert 경로 → use_certificate_chain_file 에러 반환
// ---------------------------------------------------------------------------
TEST(SslContextFailTest, InvalidCertPath_ReturnsError) {
    boost::asio::ssl::context ctx{boost::asio::ssl::context::tls_server};

    boost::system::error_code ec;
    // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
    ctx.use_certificate_chain_file("/nonexistent/path/cert.pem", ec);

    EXPECT_TRUE(ec) << "존재하지 않는 cert 경로에서 에러가 반환되어야 함";
}

// ---------------------------------------------------------------------------
// SF-2. 존재하지 않는 key 경로 → use_private_key_file 에러 반환
// ---------------------------------------------------------------------------
TEST(SslContextFailTest, InvalidKeyPath_ReturnsError) {
    boost::asio::ssl::context ctx{boost::asio::ssl::context::tls_server};

    boost::system::error_code ec;
    // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
    ctx.use_private_key_file("/nonexistent/path/key.pem", boost::asio::ssl::context::pem, ec);

    EXPECT_TRUE(ec) << "존재하지 않는 key 경로에서 에러가 반환되어야 함";
}

// ---------------------------------------------------------------------------
// SF-3. 존재하지 않는 CA 경로 → load_verify_file 에러 반환
// ---------------------------------------------------------------------------
TEST(SslContextFailTest, InvalidCaPath_ReturnsError) {
    boost::asio::ssl::context ctx{boost::asio::ssl::context::tls_client};

    boost::system::error_code ec;
    // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
    ctx.load_verify_file("/nonexistent/path/ca.pem", ec);

    EXPECT_TRUE(ec) << "존재하지 않는 CA 경로에서 에러가 반환되어야 함";
}

// ---------------------------------------------------------------------------
// SF-4. 빈 문자열을 cert 경로로 지정 → use_certificate_chain_file 에러 반환
// ---------------------------------------------------------------------------
TEST(SslContextFailTest, EmptyCertPath_ReturnsError) {
    boost::asio::ssl::context ctx{boost::asio::ssl::context::tls_server};

    boost::system::error_code ec;
    ctx.use_certificate_chain_file("", ec);  // NOLINT(bugprone-unused-return-value,cert-err33-c)

    EXPECT_TRUE(ec) << "빈 cert 경로에서 에러가 반환되어야 함";
}

// ---------------------------------------------------------------------------
// SF-5. 잘못된 PEM 형식 → use_certificate_chain_file 에러 반환
// ---------------------------------------------------------------------------
TEST(SslContextFailTest, InvalidPemContent_ReturnsError) {
    // 잘못된 PEM 내용을 임시 파일에 작성
    const auto bad_cert_path =
        write_tmp_pem("ssl_bad_cert.pem", "THIS IS NOT A VALID PEM CERTIFICATE");

    boost::asio::ssl::context ctx{boost::asio::ssl::context::tls_server};

    boost::system::error_code ec;
    // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
    ctx.use_certificate_chain_file(bad_cert_path.string(), ec);

    EXPECT_TRUE(ec) << "잘못된 PEM 내용에서 에러가 반환되어야 함";
}

// ---------------------------------------------------------------------------
// SF-6. 잘못된 key PEM 형식 → use_private_key_file 에러 반환
// ---------------------------------------------------------------------------
TEST(SslContextFailTest, InvalidKeyPemContent_ReturnsError) {
    const auto bad_key_path = write_tmp_pem("ssl_bad_key.pem", "THIS IS NOT A VALID PRIVATE KEY");

    boost::asio::ssl::context ctx{boost::asio::ssl::context::tls_server};

    boost::system::error_code ec;
    // NOLINTNEXTLINE(bugprone-unused-return-value,cert-err33-c)
    ctx.use_private_key_file(bad_key_path.string(), boost::asio::ssl::context::pem, ec);

    EXPECT_TRUE(ec) << "잘못된 key PEM 내용에서 에러가 반환되어야 함";
}

// ===========================================================================
// G. Session SSL 모드 테스트
// ===========================================================================

// ---------------------------------------------------------------------------
// SG-1. Session 생성 시 backend_ssl_ctx=nullptr → 초기 상태 kHandshaking
// ---------------------------------------------------------------------------
TEST(SessionSslModeTest, NullBackendSslCtx_InitialStateIsHandshaking) {
    boost::asio::io_context ioc;
    AsyncStream client_stream{boost::asio::ip::tcp::socket{ioc}};

    const auto server_ep =
        boost::asio::ip::tcp::endpoint{boost::asio::ip::address_v4::loopback(), 3306};

    const auto session = std::make_shared<Session>(20ULL,
                                                   std::move(client_stream),
                                                   server_ep,
                                                   nullptr,  // backend_ssl_ctx=nullptr: 평문 모드
                                                   false,
                                                   "",
                                                   make_policy_engine(),
                                                   make_logger(),
                                                   make_stats());

    EXPECT_EQ(session->state(), SessionState::kHandshaking);
}

// ---------------------------------------------------------------------------
// SG-2. Session 생성 시 backend_ssl_ctx=유효 포인터 → 초기 상태 kHandshaking
//   (SSL context 포인터를 전달해도 생성자에서는 TLS를 시작하지 않음)
// ---------------------------------------------------------------------------
TEST(SessionSslModeTest, ValidBackendSslCtx_InitialStateIsHandshaking) {
    boost::asio::io_context ioc;

    // Backend SSL context 생성
    boost::asio::ssl::context backend_ssl_ctx{boost::asio::ssl::context::tls_client};

    AsyncStream client_stream{boost::asio::ip::tcp::socket{ioc}};

    const auto server_ep =
        boost::asio::ip::tcp::endpoint{boost::asio::ip::address_v4::loopback(), 3306};

    const auto session =
        std::make_shared<Session>(21ULL,
                                  std::move(client_stream),
                                  server_ep,
                                  &backend_ssl_ctx,  // backend_ssl_ctx 유효 포인터: TLS 모드 준비
                                  false,
                                  "",
                                  make_policy_engine(),
                                  make_logger(),
                                  make_stats());

    // 생성자에서 TLS를 시작하지 않으므로 kHandshaking 상태
    EXPECT_EQ(session->state(), SessionState::kHandshaking);
}

// ---------------------------------------------------------------------------
// SG-3. TCP 소켓으로 만든 AsyncStream → is_ssl()=false (평문 모드)
// ---------------------------------------------------------------------------
TEST(SessionSslModeTest, TcpClientStream_IsNotSsl) {
    boost::asio::io_context ioc;
    const AsyncStream client_stream{boost::asio::ip::tcp::socket{ioc}};

    EXPECT_FALSE(client_stream.is_ssl());
}

// ---------------------------------------------------------------------------
// SG-4. SSL 소켓으로 만든 AsyncStream → is_ssl()=true (TLS 모드)
// ---------------------------------------------------------------------------
TEST(SessionSslModeTest, SslClientStream_IsSsl) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx{boost::asio::ssl::context::tls_server};

    AsyncStream::ssl_socket ssl_sock{boost::asio::ip::tcp::socket{ioc}, ssl_ctx};
    const AsyncStream client_stream{std::move(ssl_sock)};

    EXPECT_TRUE(client_stream.is_ssl());
}

// ---------------------------------------------------------------------------
// SG-5. Session 생성 시 backend_ssl_ctx 유효 포인터 전달 → close() 정상 동작
//   (run() 없이 close() 호출 시 크래시 없어야 함)
// ---------------------------------------------------------------------------
TEST(SessionSslModeTest, ValidBackendSslCtx_CloseIdempotent) {
    boost::asio::io_context ioc;
    boost::asio::ssl::context backend_ssl_ctx{boost::asio::ssl::context::tls_client};

    AsyncStream client_stream{boost::asio::ip::tcp::socket{ioc}};

    const auto server_ep =
        boost::asio::ip::tcp::endpoint{boost::asio::ip::address_v4::loopback(), 3306};

    const auto session = std::make_shared<Session>(22ULL,
                                                   std::move(client_stream),
                                                   server_ep,
                                                   &backend_ssl_ctx,
                                                   false,
                                                   "",
                                                   make_policy_engine(),
                                                   make_logger(),
                                                   make_stats());

    // close()는 idempotent — SSL ctx 포인터가 유효해도 크래시 없어야 함
    EXPECT_NO_THROW({
        session->close();
        session->close();
    });
}
