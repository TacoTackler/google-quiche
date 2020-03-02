// Copyright (c) 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/third_party/quiche/src/quic/core/tls_client_handshaker.h"

#include <cstring>
#include <string>

#include "third_party/boringssl/src/include/openssl/ssl.h"
#include "net/third_party/quiche/src/quic/core/crypto/quic_encrypter.h"
#include "net/third_party/quiche/src/quic/core/crypto/transport_parameters.h"
#include "net/third_party/quiche/src/quic/core/quic_session.h"
#include "net/third_party/quiche/src/common/platform/api/quiche_string_piece.h"
#include "net/third_party/quiche/src/common/platform/api/quiche_text_utils.h"

namespace quic {

TlsClientHandshaker::ProofVerifierCallbackImpl::ProofVerifierCallbackImpl(
    TlsClientHandshaker* parent)
    : parent_(parent) {}

TlsClientHandshaker::ProofVerifierCallbackImpl::~ProofVerifierCallbackImpl() {}

void TlsClientHandshaker::ProofVerifierCallbackImpl::Run(
    bool ok,
    const std::string& /*error_details*/,
    std::unique_ptr<ProofVerifyDetails>* details) {
  if (parent_ == nullptr) {
    return;
  }

  parent_->verify_details_ = std::move(*details);
  parent_->verify_result_ = ok ? ssl_verify_ok : ssl_verify_invalid;
  parent_->state_ = STATE_HANDSHAKE_RUNNING;
  parent_->proof_verify_callback_ = nullptr;
  parent_->proof_handler_->OnProofVerifyDetailsAvailable(
      *parent_->verify_details_);
  parent_->AdvanceHandshake();
}

void TlsClientHandshaker::ProofVerifierCallbackImpl::Cancel() {
  parent_ = nullptr;
}

TlsClientHandshaker::TlsClientHandshaker(
    const QuicServerId& server_id,
    QuicCryptoStream* stream,
    QuicSession* session,
    std::unique_ptr<ProofVerifyContext> verify_context,
    QuicCryptoClientConfig* crypto_config,
    QuicCryptoClientStream::ProofHandler* proof_handler)
    : TlsHandshaker(stream, session),
      session_(session),
      server_id_(server_id),
      proof_verifier_(crypto_config->proof_verifier()),
      verify_context_(std::move(verify_context)),
      proof_handler_(proof_handler),
      session_cache_(crypto_config->session_cache()),
      user_agent_id_(crypto_config->user_agent_id()),
      crypto_negotiated_params_(new QuicCryptoNegotiatedParameters),
      tls_connection_(crypto_config->ssl_ctx(), this) {}

TlsClientHandshaker::~TlsClientHandshaker() {
  if (proof_verify_callback_) {
    proof_verify_callback_->Cancel();
  }
}

bool TlsClientHandshaker::CryptoConnect() {
  state_ = STATE_HANDSHAKE_RUNNING;

  // Set the SNI to send, if any.
  SSL_set_connect_state(ssl());
  if (!server_id_.host().empty() &&
      SSL_set_tlsext_host_name(ssl(), server_id_.host().c_str()) != 1) {
    return false;
  }

  if (!SetAlpn()) {
    CloseConnection(QUIC_HANDSHAKE_FAILED, "Client failed to set ALPN");
    return false;
  }

  // Set the Transport Parameters to send in the ClientHello
  if (!SetTransportParameters()) {
    CloseConnection(QUIC_HANDSHAKE_FAILED,
                    "Client failed to set Transport Parameters");
    return false;
  }

  // Set a session to resume, if there is one.
  if (session_cache_) {
    std::unique_ptr<QuicResumptionState> cached_state =
        session_cache_->Lookup(server_id_, SSL_get_SSL_CTX(ssl()));
    if (cached_state) {
      SSL_set_session(ssl(), cached_state->tls_session.get());
    }
  }

  // Start the handshake.
  AdvanceHandshake();
  return session()->connection()->connected();
}

static bool IsValidAlpn(const std::string& alpn_string) {
  return alpn_string.length() <= std::numeric_limits<uint8_t>::max();
}

bool TlsClientHandshaker::SetAlpn() {
  std::vector<std::string> alpns = session()->GetAlpnsToOffer();
  if (alpns.empty()) {
    if (allow_empty_alpn_for_tests_) {
      return true;
    }

    QUIC_BUG << "ALPN missing";
    return false;
  }
  if (!std::all_of(alpns.begin(), alpns.end(), IsValidAlpn)) {
    QUIC_BUG << "ALPN too long";
    return false;
  }

  // SSL_set_alpn_protos expects a sequence of one-byte-length-prefixed
  // strings.
  uint8_t alpn[1024];
  QuicDataWriter alpn_writer(sizeof(alpn), reinterpret_cast<char*>(alpn));
  bool success = true;
  for (const std::string& alpn_string : alpns) {
    success = success && alpn_writer.WriteUInt8(alpn_string.size()) &&
              alpn_writer.WriteStringPiece(alpn_string);
  }
  success =
      success && (SSL_set_alpn_protos(ssl(), alpn, alpn_writer.length()) == 0);
  if (!success) {
    QUIC_BUG << "Failed to set ALPN: "
             << quiche::QuicheTextUtils::HexDump(quiche::QuicheStringPiece(
                    alpn_writer.data(), alpn_writer.length()));
    return false;
  }
  QUIC_DLOG(INFO) << "Client using ALPN: '" << alpns[0] << "'";
  return true;
}

bool TlsClientHandshaker::SetTransportParameters() {
  TransportParameters params;
  params.perspective = Perspective::IS_CLIENT;
  params.version =
      CreateQuicVersionLabel(session()->supported_versions().front());

  if (!session()->config()->FillTransportParameters(&params)) {
    return false;
  }
  params.google_quic_params->SetStringPiece(kUAID, user_agent_id_);

  std::vector<uint8_t> param_bytes;
  return SerializeTransportParameters(session()->connection()->version(),
                                      params, &param_bytes) &&
         SSL_set_quic_transport_params(ssl(), param_bytes.data(),
                                       param_bytes.size()) == 1;
}

bool TlsClientHandshaker::ProcessTransportParameters(
    std::string* error_details) {
  TransportParameters params;
  const uint8_t* param_bytes;
  size_t param_bytes_len;
  SSL_get_peer_quic_transport_params(ssl(), &param_bytes, &param_bytes_len);
  if (param_bytes_len == 0) {
    *error_details = "Server's transport parameters are missing";
    return false;
  }
  std::string parse_error_details;
  if (!ParseTransportParameters(
          session()->connection()->version(), Perspective::IS_SERVER,
          param_bytes, param_bytes_len, &params, &parse_error_details)) {
    DCHECK(!parse_error_details.empty());
    *error_details =
        "Unable to parse server's transport parameters: " + parse_error_details;
    return false;
  }

  // When interoperating with non-Google implementations that do not send
  // the version extension, set it to what we expect.
  if (params.version == 0) {
    params.version = CreateQuicVersionLabel(session()->connection()->version());
  }
  if (params.supported_versions.empty()) {
    params.supported_versions.push_back(params.version);
  }

  if (params.version !=
      CreateQuicVersionLabel(session()->connection()->version())) {
    *error_details = "Version mismatch detected";
    return false;
  }
  if (CryptoUtils::ValidateServerHelloVersions(
          params.supported_versions,
          session()->connection()->server_supported_versions(),
          error_details) != QUIC_NO_ERROR ||
      session()->config()->ProcessTransportParameters(
          params, SERVER, error_details) != QUIC_NO_ERROR) {
    DCHECK(!error_details->empty());
    return false;
  }

  session()->OnConfigNegotiated();
  return true;
}

int TlsClientHandshaker::num_sent_client_hellos() const {
  return 0;
}

bool TlsClientHandshaker::IsResumption() const {
  QUIC_BUG_IF(!one_rtt_keys_available_);
  return SSL_session_reused(ssl()) == 1;
}

bool TlsClientHandshaker::EarlyDataAccepted() const {
  QUIC_BUG_IF(!one_rtt_keys_available_);
  return SSL_early_data_accepted(ssl()) == 1;
}

bool TlsClientHandshaker::ReceivedInchoateReject() const {
  QUIC_BUG_IF(!one_rtt_keys_available_);
  // REJ messages are a QUIC crypto feature, so TLS always returns false.
  return false;
}

int TlsClientHandshaker::num_scup_messages_received() const {
  // SCUP messages aren't sent or received when using the TLS handshake.
  return 0;
}

std::string TlsClientHandshaker::chlo_hash() const {
  return "";
}

bool TlsClientHandshaker::encryption_established() const {
  return encryption_established_;
}

bool TlsClientHandshaker::one_rtt_keys_available() const {
  return one_rtt_keys_available_;
}

const QuicCryptoNegotiatedParameters&
TlsClientHandshaker::crypto_negotiated_params() const {
  return *crypto_negotiated_params_;
}

CryptoMessageParser* TlsClientHandshaker::crypto_message_parser() {
  return TlsHandshaker::crypto_message_parser();
}

HandshakeState TlsClientHandshaker::GetHandshakeState() const {
  if (handshake_confirmed_) {
    return HANDSHAKE_CONFIRMED;
  }
  if (one_rtt_keys_available_) {
    return HANDSHAKE_COMPLETE;
  }
  if (state_ >= STATE_ENCRYPTION_HANDSHAKE_DATA_SENT) {
    return HANDSHAKE_PROCESSED;
  }
  return HANDSHAKE_START;
}

size_t TlsClientHandshaker::BufferSizeLimitForLevel(
    EncryptionLevel level) const {
  return TlsHandshaker::BufferSizeLimitForLevel(level);
}

void TlsClientHandshaker::OnOneRttPacketAcknowledged() {
  OnHandshakeConfirmed();
}

void TlsClientHandshaker::OnHandshakeDoneReceived() {
  if (!one_rtt_keys_available_) {
    CloseConnection(QUIC_HANDSHAKE_FAILED,
                    "Unexpected handshake done received");
    return;
  }
  OnHandshakeConfirmed();
}

void TlsClientHandshaker::OnHandshakeConfirmed() {
  DCHECK(one_rtt_keys_available_);
  if (handshake_confirmed_) {
    return;
  }
  handshake_confirmed_ = true;
  handshaker_delegate()->DiscardOldEncryptionKey(ENCRYPTION_HANDSHAKE);
  handshaker_delegate()->DiscardOldDecryptionKey(ENCRYPTION_HANDSHAKE);
}

void TlsClientHandshaker::AdvanceHandshake() {
  if (state_ == STATE_CONNECTION_CLOSED) {
    QUIC_LOG(INFO)
        << "TlsClientHandshaker received message after connection closed";
    return;
  }
  if (state_ == STATE_IDLE) {
    CloseConnection(QUIC_HANDSHAKE_FAILED,
                    "Client observed TLS handshake idle failure");
    return;
  }
  if (state_ == STATE_HANDSHAKE_COMPLETE) {
    int rv = SSL_process_quic_post_handshake(ssl());
    if (rv != 1) {
      CloseConnection(QUIC_HANDSHAKE_FAILED, "Unexpected post-handshake data");
    }
    return;
  }

  QUIC_LOG(INFO) << "TlsClientHandshaker: continuing handshake";
  int rv = SSL_do_handshake(ssl());
  if (rv == 1) {
    FinishHandshake();
    return;
  }
  int ssl_error = SSL_get_error(ssl(), rv);
  bool should_close = true;
  switch (state_) {
    case STATE_HANDSHAKE_RUNNING:
      should_close = ssl_error != SSL_ERROR_WANT_READ;
      break;
    case STATE_CERT_VERIFY_PENDING:
      should_close = ssl_error != SSL_ERROR_WANT_CERTIFICATE_VERIFY;
      break;
    default:
      should_close = true;
  }
  if (should_close && state_ != STATE_CONNECTION_CLOSED) {
    // TODO(nharper): Surface error details from the error queue when ssl_error
    // is SSL_ERROR_SSL.
    QUIC_LOG(WARNING) << "SSL_do_handshake failed; closing connection";
    CloseConnection(QUIC_HANDSHAKE_FAILED,
                    "Client observed TLS handshake failure");
  }
}

void TlsClientHandshaker::CloseConnection(QuicErrorCode error,
                                          const std::string& reason_phrase) {
  DCHECK(!reason_phrase.empty());
  state_ = STATE_CONNECTION_CLOSED;
  stream()->OnUnrecoverableError(error, reason_phrase);
}

void TlsClientHandshaker::FinishHandshake() {
  QUIC_LOG(INFO) << "Client: handshake finished";
  state_ = STATE_HANDSHAKE_COMPLETE;

  std::string error_details;
  if (!ProcessTransportParameters(&error_details)) {
    DCHECK(!error_details.empty());
    CloseConnection(QUIC_HANDSHAKE_FAILED, error_details);
    return;
  }

  const uint8_t* alpn_data = nullptr;
  unsigned alpn_length = 0;
  SSL_get0_alpn_selected(ssl(), &alpn_data, &alpn_length);

  if (alpn_length == 0) {
    QUIC_DLOG(ERROR) << "Client: server did not select ALPN";
    // TODO(b/130164908) this should send no_application_protocol
    // instead of QUIC_HANDSHAKE_FAILED.
    CloseConnection(QUIC_HANDSHAKE_FAILED, "Server did not select ALPN");
    return;
  }

  std::string received_alpn_string(reinterpret_cast<const char*>(alpn_data),
                                   alpn_length);
  std::vector<std::string> offered_alpns = session()->GetAlpnsToOffer();
  if (std::find(offered_alpns.begin(), offered_alpns.end(),
                received_alpn_string) == offered_alpns.end()) {
    QUIC_LOG(ERROR) << "Client: received mismatched ALPN '"
                    << received_alpn_string;
    // TODO(b/130164908) this should send no_application_protocol
    // instead of QUIC_HANDSHAKE_FAILED.
    CloseConnection(QUIC_HANDSHAKE_FAILED, "Client received mismatched ALPN");
    return;
  }
  session()->OnAlpnSelected(received_alpn_string);
  QUIC_DLOG(INFO) << "Client: server selected ALPN: '" << received_alpn_string
                  << "'";

  encryption_established_ = true;
  one_rtt_keys_available_ = true;

  // Fill crypto_negotiated_params_:
  const SSL_CIPHER* cipher = SSL_get_current_cipher(ssl());
  if (cipher) {
    crypto_negotiated_params_->cipher_suite = SSL_CIPHER_get_value(cipher);
  }
  crypto_negotiated_params_->key_exchange_group = SSL_get_curve_id(ssl());
  crypto_negotiated_params_->peer_signature_algorithm =
      SSL_get_peer_signature_algorithm(ssl());

  handshaker_delegate()->SetDefaultEncryptionLevel(ENCRYPTION_FORWARD_SECURE);
}

enum ssl_verify_result_t TlsClientHandshaker::VerifyCert(uint8_t* out_alert) {
  if (verify_result_ != ssl_verify_retry ||
      state_ == STATE_CERT_VERIFY_PENDING) {
    enum ssl_verify_result_t result = verify_result_;
    verify_result_ = ssl_verify_retry;
    return result;
  }
  const STACK_OF(CRYPTO_BUFFER)* cert_chain = SSL_get0_peer_certificates(ssl());
  if (cert_chain == nullptr) {
    *out_alert = SSL_AD_INTERNAL_ERROR;
    return ssl_verify_invalid;
  }
  // TODO(nharper): Pass the CRYPTO_BUFFERs into the QUIC stack to avoid copies.
  std::vector<std::string> certs;
  for (CRYPTO_BUFFER* cert : cert_chain) {
    certs.push_back(
        std::string(reinterpret_cast<const char*>(CRYPTO_BUFFER_data(cert)),
                    CRYPTO_BUFFER_len(cert)));
  }
  const uint8_t* ocsp_response_raw;
  size_t ocsp_response_len;
  SSL_get0_ocsp_response(ssl(), &ocsp_response_raw, &ocsp_response_len);
  std::string ocsp_response(reinterpret_cast<const char*>(ocsp_response_raw),
                            ocsp_response_len);
  const uint8_t* sct_list_raw;
  size_t sct_list_len;
  SSL_get0_signed_cert_timestamp_list(ssl(), &sct_list_raw, &sct_list_len);
  std::string sct_list(reinterpret_cast<const char*>(sct_list_raw),
                       sct_list_len);

  ProofVerifierCallbackImpl* proof_verify_callback =
      new ProofVerifierCallbackImpl(this);

  QuicAsyncStatus verify_result = proof_verifier_->VerifyCertChain(
      server_id_.host(), certs, ocsp_response, sct_list, verify_context_.get(),
      &cert_verify_error_details_, &verify_details_,
      std::unique_ptr<ProofVerifierCallback>(proof_verify_callback));
  switch (verify_result) {
    case QUIC_SUCCESS:
      proof_handler_->OnProofVerifyDetailsAvailable(*verify_details_);
      return ssl_verify_ok;
    case QUIC_PENDING:
      proof_verify_callback_ = proof_verify_callback;
      state_ = STATE_CERT_VERIFY_PENDING;
      return ssl_verify_retry;
    case QUIC_FAILURE:
    default:
      QUIC_LOG(INFO) << "Cert chain verification failed: "
                     << cert_verify_error_details_;
      return ssl_verify_invalid;
  }
}

void TlsClientHandshaker::InsertSession(bssl::UniquePtr<SSL_SESSION> session) {
  if (session_cache_ == nullptr) {
    QUIC_DVLOG(1) << "No session cache, not inserting a session";
    return;
  }
  auto cache_state = std::make_unique<QuicResumptionState>();
  cache_state->tls_session = std::move(session);
  session_cache_->Insert(server_id_, std::move(cache_state));
}

void TlsClientHandshaker::WriteMessage(EncryptionLevel level,
                                       quiche::QuicheStringPiece data) {
  if (level == ENCRYPTION_HANDSHAKE &&
      state_ < STATE_ENCRYPTION_HANDSHAKE_DATA_SENT) {
    state_ = STATE_ENCRYPTION_HANDSHAKE_DATA_SENT;
    handshaker_delegate()->DiscardOldEncryptionKey(ENCRYPTION_INITIAL);
    handshaker_delegate()->DiscardOldDecryptionKey(ENCRYPTION_INITIAL);
  }
  TlsHandshaker::WriteMessage(level, data);
}

}  // namespace quic
