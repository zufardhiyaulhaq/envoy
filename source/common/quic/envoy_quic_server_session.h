#pragma once

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif

#include "quiche/quic/core/http/quic_server_session_base.h"
#include "quiche/quic/core/quic_crypto_server_stream.h"
#include "quiche/quic/core/tls_server_handshaker.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <memory>

#include "common/quic/quic_filter_manager_connection_impl.h"
#include "common/quic/envoy_quic_server_stream.h"

namespace Envoy {
namespace Quic {

// Act as a Network::Connection to HCM and a FilterManager to FilterFactoryCb.
// TODO(danzh) Lifetime of quic connection and filter manager connection can be
// simplified by changing the inheritance to a member variable instantiated
// before quic_connection_.
class EnvoyQuicServerSession : public quic::QuicServerSessionBase,
                               public QuicFilterManagerConnectionImpl {
public:
  EnvoyQuicServerSession(const quic::QuicConfig& config,
                         const quic::ParsedQuicVersionVector& supported_versions,
                         std::unique_ptr<EnvoyQuicConnection> connection,
                         quic::QuicSession::Visitor* visitor,
                         quic::QuicCryptoServerStreamBase::Helper* helper,
                         const quic::QuicCryptoServerConfig* crypto_config,
                         quic::QuicCompressedCertsCache* compressed_certs_cache,
                         Event::Dispatcher& dispatcher, uint32_t send_buffer_limit,
                         Network::ListenerConfig& listener_config);

  ~EnvoyQuicServerSession() override;

  // Network::Connection
  absl::string_view requestedServerName() const override;

  // Called by QuicHttpServerConnectionImpl before creating data streams.
  void setHttpConnectionCallbacks(Http::ServerConnectionCallbacks& callbacks) {
    http_connection_callbacks_ = &callbacks;
  }

  // quic::QuicSession
  void OnConnectionClosed(const quic::QuicConnectionCloseFrame& frame,
                          quic::ConnectionCloseSource source) override;
  void Initialize() override;
  void OnCanWrite() override;
  void OnTlsHandshakeComplete() override;
  // quic::QuicSpdySession
  void SetDefaultEncryptionLevel(quic::EncryptionLevel level) override;

  using quic::QuicSession::PerformActionOnActiveStreams;

protected:
  // quic::QuicServerSessionBase
  std::unique_ptr<quic::QuicCryptoServerStreamBase>
  CreateQuicCryptoServerStream(const quic::QuicCryptoServerConfig* crypto_config,
                               quic::QuicCompressedCertsCache* compressed_certs_cache) override;

  // quic::QuicSession
  // Overridden to create stream as encoder and associate it with an decoder.
  quic::QuicSpdyStream* CreateIncomingStream(quic::QuicStreamId id) override;
  quic::QuicSpdyStream* CreateIncomingStream(quic::PendingStream* pending) override;
  quic::QuicSpdyStream* CreateOutgoingBidirectionalStream() override;
  quic::QuicSpdyStream* CreateOutgoingUnidirectionalStream() override;

  // QuicFilterManagerConnectionImpl
  bool hasDataToWrite() override;

private:
  void setUpRequestDecoder(EnvoyQuicServerStream& stream);
  void maybeCreateNetworkFilters();

  std::unique_ptr<EnvoyQuicConnection> quic_connection_;
  Network::ListenerConfig& listener_config_;
  // These callbacks are owned by network filters and quic session should out live
  // them.
  Http::ServerConnectionCallbacks* http_connection_callbacks_{nullptr};
};

} // namespace Quic
} // namespace Envoy
