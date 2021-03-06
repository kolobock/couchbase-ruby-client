/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2020 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#pragma once

#include <utility>

#include <tao/json.hpp>

#include <asio.hpp>

#include <platform/uuid.h>

#include <io/mcbp_message.hxx>
#include <io/mcbp_parser.hxx>

#include <timeout_defaults.hxx>

#include <protocol/hello_feature.hxx>
#include <protocol/client_request.hxx>
#include <protocol/client_response.hxx>
#include <protocol/server_request.hxx>
#include <protocol/cmd_hello.hxx>
#include <protocol/cmd_sasl_list_mechs.hxx>
#include <protocol/cmd_sasl_auth.hxx>
#include <protocol/cmd_sasl_step.hxx>
#include <protocol/cmd_select_bucket.hxx>
#include <protocol/cmd_get_cluster_config.hxx>
#include <protocol/cmd_get_error_map.hxx>
#include <protocol/cmd_get.hxx>
#include <protocol/cmd_cluster_map_change_notification.hxx>

#include <cbsasl/client.h>

#include <spdlog/fmt/bin_to_hex.h>

#include <origin.hxx>
#include <errors.hxx>
#include <version.hxx>

namespace couchbase::io
{

class mcbp_session : public std::enable_shared_from_this<mcbp_session>
{
    class collection_cache
    {
      private:
        std::map<std::string, std::uint32_t> cid_map_{ { "_default._default", 0 } };

      public:
        [[nodiscard]] std::optional<std::uint32_t> get(const std::string& path)
        {
            Expects(!path.empty());
            auto ptr = cid_map_.find(path);
            if (ptr != cid_map_.end()) {
                return ptr->second;
            }
            return {};
        }

        void update(const std::string& path, std::uint32_t id)
        {
            Expects(!path.empty());
            cid_map_[path] = id;
        }

        void reset()
        {
            cid_map_.clear();
            cid_map_["_default._default"] = 0;
        }
    };

    class message_handler
    {
      public:
        virtual void handle(mcbp_message&& msg) = 0;

        virtual ~message_handler() = default;

        virtual void stop()
        {
        }
    };

    class bootstrap_handler : public message_handler
    {
      private:
        std::shared_ptr<mcbp_session> session_;
        sasl::ClientContext sasl_;
        std::atomic_bool stopped_{ false };

      public:
        ~bootstrap_handler() override = default;

        void stop() override
        {
            if (stopped_) {
                return;
            }
            stopped_ = true;
            session_.reset();
        }

        explicit bootstrap_handler(std::shared_ptr<mcbp_session> session)
          : session_(session)
          , sasl_([origin = session_->origin_]() -> std::string { return origin.get_username(); },
                  [origin = session_->origin_]() -> std::string { return origin.get_password(); },
                  { "SCRAM-SHA512", "SCRAM-SHA256", "SCRAM-SHA1", "PLAIN" })
        {
            tao::json::value user_agent{
                { "a",
                  fmt::format(
                    "ruby/{}.{}.{}/{}", BACKEND_VERSION_MAJOR, BACKEND_VERSION_MINOR, BACKEND_VERSION_PATCH, BACKEND_GIT_REVISION) },
                { "i", fmt::format("{}/{}", session_->client_id_, session_->id_) }
            };
            protocol::client_request<protocol::hello_request_body> hello_req;
            hello_req.opaque(session_->next_opaque());
            hello_req.body().user_agent(tao::json::to_string(user_agent));
            spdlog::debug("{} user_agent={}, requested_features=[{}]",
                          session_->log_prefix_,
                          hello_req.body().user_agent(),
                          fmt::join(hello_req.body().features(), ", "));
            session_->write(hello_req.data());

            protocol::client_request<protocol::sasl_list_mechs_request_body> list_req;
            list_req.opaque(session_->next_opaque());
            session_->write(list_req.data());

            protocol::client_request<protocol::sasl_auth_request_body> auth_req;
            sasl::error sasl_code;
            std::string_view sasl_payload;
            std::tie(sasl_code, sasl_payload) = sasl_.start();
            auth_req.opaque(session_->next_opaque());
            auth_req.body().mechanism(sasl_.get_name());
            auth_req.body().sasl_data(sasl_payload);
            session_->write(auth_req.data());

            session_->flush();
        }

        void complete(std::error_code ec)
        {
            stopped_ = true;
            session_->invoke_bootstrap_handler(ec);
        }

        void auth_success()
        {
            session_->authenticated_ = true;
            if (session_->supports_feature(protocol::hello_feature::xerror)) {
                protocol::client_request<protocol::get_error_map_request_body> errmap_req;
                errmap_req.opaque(session_->next_opaque());
                session_->write(errmap_req.data());
            }
            if (session_->bucket_name_) {
                protocol::client_request<protocol::select_bucket_request_body> sb_req;
                sb_req.opaque(session_->next_opaque());
                sb_req.body().bucket_name(session_->bucket_name_.value());
                session_->write(sb_req.data());
            }
            protocol::client_request<protocol::get_cluster_config_request_body> cfg_req;
            cfg_req.opaque(session_->next_opaque());
            session_->write(cfg_req.data());
            session_->flush();
        }

        void handle(mcbp_message&& msg) override
        {
            if (stopped_ || !session_) {
                return;
            }
            Expects(protocol::is_valid_client_opcode(msg.header.opcode));
            auto opcode = static_cast<protocol::client_opcode>(msg.header.opcode);
            switch (opcode) {
                case protocol::client_opcode::hello: {
                    protocol::client_response<protocol::hello_response_body> resp(msg);
                    if (resp.status() == protocol::status::success) {
                        session_->supported_features_ = resp.body().supported_features();
                        spdlog::debug("{} supported_features=[{}]", session_->log_prefix_, fmt::join(session_->supported_features_, ", "));
                    } else {
                        spdlog::warn("{} unexpected message status during bootstrap: {}", session_->log_prefix_, resp.error_message());
                        return complete(std::make_error_code(error::network_errc::handshake_failure));
                    }
                } break;
                case protocol::client_opcode::sasl_list_mechs: {
                    protocol::client_response<protocol::sasl_list_mechs_response_body> resp(msg);
                    if (resp.status() != protocol::status::success) {
                        spdlog::warn("{} unexpected message status during bootstrap: {}", session_->log_prefix_, resp.error_message());
                        return complete(std::make_error_code(error::common_errc::authentication_failure));
                    }
                } break;
                case protocol::client_opcode::sasl_auth: {
                    protocol::client_response<protocol::sasl_auth_response_body> resp(msg);
                    if (resp.status() == protocol::status::success) {
                        return auth_success();
                    }
                    if (resp.status() == protocol::status::auth_continue) {
                        sasl::error sasl_code;
                        std::string_view sasl_payload;
                        std::tie(sasl_code, sasl_payload) = sasl_.step(resp.body().value());
                        if (sasl_code == sasl::error::OK) {
                            return auth_success();
                        }
                        if (sasl_code == sasl::error::CONTINUE) {
                            protocol::client_request<protocol::sasl_step_request_body> req;
                            req.opaque(session_->next_opaque());
                            req.body().mechanism(sasl_.get_name());
                            req.body().sasl_data(sasl_payload);
                            session_->write_and_flush(req.data());
                        } else {
                            spdlog::error("{} unable to authenticate: sasl_code={}", session_->log_prefix_, sasl_code);
                            return complete(std::make_error_code(error::common_errc::authentication_failure));
                        }
                    } else {
                        spdlog::warn("{} unexpected message status during bootstrap: {} (opcode={})",
                                     session_->log_prefix_,
                                     resp.error_message(),
                                     opcode);
                        return complete(std::make_error_code(error::common_errc::authentication_failure));
                    }
                } break;
                case protocol::client_opcode::sasl_step: {
                    protocol::client_response<protocol::sasl_step_response_body> resp(msg);
                    if (resp.status() == protocol::status::success) {
                        return auth_success();
                    }
                    return complete(std::make_error_code(error::common_errc::authentication_failure));
                }
                case protocol::client_opcode::get_error_map: {
                    protocol::client_response<protocol::get_error_map_response_body> resp(msg);
                    if (resp.status() == protocol::status::success) {
                        session_->errmap_.emplace(resp.body().errmap());
                    } else {
                        spdlog::warn("{} unexpected message status during bootstrap: {} (opcode={})",
                                     session_->log_prefix_,
                                     resp.error_message(),
                                     opcode);
                        return complete(std::make_error_code(error::network_errc::protocol_error));
                    }
                } break;
                case protocol::client_opcode::select_bucket: {
                    protocol::client_response<protocol::select_bucket_response_body> resp(msg);
                    if (resp.status() == protocol::status::success) {
                        spdlog::debug("{} selected bucket: {}", session_->log_prefix_, session_->bucket_name_.value_or(""));
                        session_->bucket_selected_ = true;
                    } else if (resp.status() == protocol::status::no_access) {
                        spdlog::debug("{} unable to select bucket: {}, probably the bucket does not exist",
                                      session_->log_prefix_,
                                      session_->bucket_name_.value_or(""));
                        session_->bucket_selected_ = false;
                        return complete(std::make_error_code(error::common_errc::bucket_not_found));
                    } else {
                        spdlog::warn("{} unexpected message status during bootstrap: {}", session_->log_prefix_, resp.error_message());
                        return complete(std::make_error_code(error::common_errc::bucket_not_found));
                    }
                } break;
                case protocol::client_opcode::get_cluster_config: {
                    protocol::client_response<protocol::get_cluster_config_response_body> resp(msg);
                    if (resp.status() == protocol::status::success) {
                        session_->update_configuration(resp.body().config());
                        complete({});
                    } else if (resp.status() == protocol::status::no_bucket && !session_->bucket_name_) {
                        // bucket-less session, but the server wants bucket
                        session_->supports_gcccp_ = false;
                        spdlog::warn("{} this server does not support GCCCP, open bucket before making any cluster-level command",
                                     session_->log_prefix_);
                        session_->update_configuration(
                          make_blank_configuration(session_->endpoint_address_, session_->endpoint_.port(), 0));
                        complete({});
                    } else {
                        spdlog::warn("{} unexpected message status during bootstrap: {} (opcode={})",
                                     session_->log_prefix_,
                                     resp.error_message(),
                                     opcode);
                        return complete(std::make_error_code(error::network_errc::protocol_error));
                    }
                } break;
                default:
                    spdlog::warn("{} unexpected message during bootstrap: {}", session_->log_prefix_, opcode);
                    return complete(std::make_error_code(error::network_errc::protocol_error));
            }
        }
    };

    class normal_handler : public message_handler
    {
      private:
        std::shared_ptr<mcbp_session> session_;
        asio::steady_timer heartbeat_timer_;
        std::atomic_bool stopped_{ false };

      public:
        ~normal_handler() override = default;

        explicit normal_handler(std::shared_ptr<mcbp_session> session)
          : session_(session)
          , heartbeat_timer_(session_->ctx_)
        {
            if (session_->supports_gcccp_) {
                fetch_config({});
            }
        }

        void stop() override
        {
            if (stopped_) {
                return;
            }
            stopped_ = true;
            heartbeat_timer_.cancel();
            session_.reset();
        }

        void handle(mcbp_message&& msg) override
        {
            if (stopped_ || !session_) {
                return;
            }
            Expects(protocol::is_valid_magic(msg.header.magic));
            switch (auto magic = static_cast<protocol::magic>(msg.header.magic)) {
                case protocol::magic::client_response:
                case protocol::magic::alt_client_response:
                    Expects(protocol::is_valid_client_opcode(msg.header.opcode));
                    switch (auto opcode = static_cast<protocol::client_opcode>(msg.header.opcode)) {
                        case protocol::client_opcode::get_cluster_config: {
                            protocol::client_response<protocol::get_cluster_config_response_body> resp(msg);
                            if (resp.status() == protocol::status::success) {
                                if (session_) {
                                    session_->update_configuration(resp.body().config());
                                }
                            } else {
                                spdlog::warn("{} unexpected message status: {}", session_->log_prefix_, resp.error_message());
                            }
                        } break;
                        case protocol::client_opcode::get_collection_id:
                        case protocol::client_opcode::get:
                        case protocol::client_opcode::get_and_lock:
                        case protocol::client_opcode::get_and_touch:
                        case protocol::client_opcode::touch:
                        case protocol::client_opcode::insert:
                        case protocol::client_opcode::replace:
                        case protocol::client_opcode::upsert:
                        case protocol::client_opcode::remove:
                        case protocol::client_opcode::observe:
                        case protocol::client_opcode::unlock:
                        case protocol::client_opcode::increment:
                        case protocol::client_opcode::decrement:
                        case protocol::client_opcode::subdoc_multi_lookup:
                        case protocol::client_opcode::subdoc_multi_mutation: {
                            std::uint32_t opaque = msg.header.opaque;
                            std::uint16_t status = ntohs(msg.header.specific);
                            auto handler = session_->command_handlers_.find(opaque);
                            if (handler != session_->command_handlers_.end()) {
                                auto ec = session_->map_status_code(opcode, status);
                                spdlog::debug("{} MCBP invoke operation handler, opaque={}, status={}, ec={}",
                                              session_->log_prefix_,
                                              opaque,
                                              status,
                                              ec.message());
                                auto fun = handler->second;
                                session_->command_handlers_.erase(handler);
                                fun(ec, std::move(msg));
                            } else {
                                spdlog::debug("{} unexpected orphan response opcode={}, opaque={}",
                                              session_->log_prefix_,
                                              msg.header.opcode,
                                              msg.header.opaque);
                            }
                        } break;
                        default:
                            spdlog::warn("{} unexpected client response: {}", session_->log_prefix_, opcode);
                    }
                    break;
                case protocol::magic::server_request:
                    Expects(protocol::is_valid_server_request_opcode(msg.header.opcode));
                    switch (auto opcode = static_cast<protocol::server_opcode>(msg.header.opcode)) {
                        case protocol::server_opcode::cluster_map_change_notification: {
                            protocol::server_request<protocol::cluster_map_change_notification_request_body> req(msg);
                            if (session_) {
                                if ((!req.body().config().bucket.has_value() && req.body().bucket().empty()) ||
                                    (session_->bucket_name_.has_value() && !req.body().bucket().empty() &&
                                     session_->bucket_name_.value() == req.body().bucket())) {
                                    session_->update_configuration(req.body().config());
                                }
                            }
                        } break;
                        default:
                            spdlog::warn("{} unexpected server request: {}", session_->log_prefix_, opcode);
                    }
                    break;
                case protocol::magic::client_request:
                case protocol::magic::alt_client_request:
                case protocol::magic::server_response:
                    spdlog::warn(
                      "{} unexpected magic: {}, opcode={}, opaque={}", session_->log_prefix_, magic, msg.header.opcode, msg.header.opaque);
                    break;
            }
        }

        void fetch_config(std::error_code ec)
        {
            if (ec == asio::error::operation_aborted || stopped_ || !session_) {
                return;
            }
            protocol::client_request<protocol::get_cluster_config_request_body> req;
            req.opaque(session_->next_opaque());
            session_->write_and_flush(req.data());
            heartbeat_timer_.expires_after(std::chrono::milliseconds(2500));
            heartbeat_timer_.async_wait(std::bind(&normal_handler::fetch_config, this, std::placeholders::_1));
        }
    };

  public:
    mcbp_session() = delete;
    mcbp_session(const std::string& client_id,
                 asio::io_context& ctx,
                 const couchbase::origin& origin,
                 std::optional<std::string> bucket_name = {},
                 std::vector<protocol::hello_feature> known_features = {})
      : client_id_(client_id)
      , id_(uuid::to_string(uuid::random()))
      , ctx_(ctx)
      , resolver_(ctx_)
      , strand_(asio::make_strand(ctx_))
      , socket_(strand_)
      , bootstrap_deadline_(ctx_)
      , connection_deadline_(ctx_)
      , retry_backoff_(ctx_)
      , origin_(origin)
      , bucket_name_(std::move(bucket_name))
      , supported_features_(known_features)
    {
        log_prefix_ = fmt::format("[{}/{}/{}]", client_id_, id_, bucket_name_.value_or("-"));
    }

    ~mcbp_session()
    {
        stop();
    }

    [[nodiscard]] const std::string& log_prefix() const
    {
        return log_prefix_;
    }

    void bootstrap(std::function<void(std::error_code, configuration)>&& handler)
    {
        bootstrap_handler_ = std::move(handler);
        bootstrap_deadline_.expires_after(timeout_defaults::bootstrap_timeout);
        bootstrap_deadline_.async_wait([self = shared_from_this()](std::error_code ec) {
            if (ec == asio::error::operation_aborted || self->stopped_) {
                return;
            }
            spdlog::warn("{} unable to bootstrap in time", self->log_prefix_);
            self->bootstrap_handler_(std::make_error_code(error::common_errc::unambiguous_timeout), {});
            self->bootstrap_handler_ = nullptr;
            self->stop();
        });
        initiate_bootstrap();
    }

    void initiate_bootstrap()
    {
        if (stopped_) {
            return;
        }
        if (origin_.exhausted()) {
            auto backoff = std::chrono::milliseconds(500);
            spdlog::debug("{} reached the end of list of bootstrap nodes, waiting for {}ms before restart", log_prefix_, backoff.count());
            retry_backoff_.expires_after(backoff);
            retry_backoff_.async_wait([self = shared_from_this()](std::error_code ec) mutable {
                if (ec == asio::error::operation_aborted || self->stopped_) {
                    return;
                }
                self->origin_.restart();
                self->initiate_bootstrap();
            });
            return;
        }
        std::string hostname;
        std::string service;
        std::tie(hostname, service) = origin_.next_address();
        log_prefix_ = fmt::format("[{}/{}/{}] <{}:{}>", client_id_, id_, bucket_name_.value_or("-"), hostname, service);
        spdlog::debug("{} attempt to establish MCBP connection", log_prefix_);
        resolver_.async_resolve(
          hostname, service, std::bind(&mcbp_session::on_resolve, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
    }

    [[nodiscard]] const std::string& id() const
    {
        return id_;
    }

    void stop()
    {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        bootstrap_deadline_.cancel();
        connection_deadline_.cancel();
        retry_backoff_.cancel();
        resolver_.cancel();
        if (socket_.is_open()) {
            socket_.close();
        }
        auto ec = std::make_error_code(error::common_errc::request_canceled);
        if (!bootstrapped_ && bootstrap_handler_) {
            bootstrap_handler_(ec, {});
            bootstrap_handler_ = nullptr;
        }
        if (handler_) {
            handler_->stop();
        }
        for (auto& handler : command_handlers_) {
            spdlog::debug("{} MCBP cancel operation during session close, opaque={}, ec={}", log_prefix_, handler.first, ec.message());
            handler.second(ec, {});
        }
        command_handlers_.clear();
    }

    void write(const std::vector<uint8_t>& buf)
    {
        if (stopped_) {
            return;
        }
        std::uint32_t opaque{ 0 };
        std::memcpy(&opaque, buf.data() + 12, sizeof(opaque));
        spdlog::debug("{} MCBP send, opaque={}, {:n}", log_prefix_, opaque, spdlog::to_hex(buf.begin(), buf.begin() + 24));
        SPDLOG_TRACE("{} MCBP send, opaque={}{:a}", log_prefix_, opaque, spdlog::to_hex(data));
        std::scoped_lock lock(output_buffer_mutex_);
        output_buffer_.push_back(buf);
    }

    void flush()
    {
        if (stopped_) {
            return;
        }
        do_write();
    }

    void write_and_flush(const std::vector<uint8_t>& buf)
    {
        if (stopped_) {
            return;
        }
        write(buf);
        flush();
    }

    void write_and_subscribe(uint32_t opaque,
                             std::vector<std::uint8_t>& data,
                             std::function<void(std::error_code, io::mcbp_message&&)> handler)
    {
        if (stopped_) {
            spdlog::warn("{} MCBP cancel operation, while trying to write to closed session opaque={}", log_prefix_, opaque);
            handler(std::make_error_code(error::common_errc::request_canceled), {});
            return;
        }
        command_handlers_.emplace(opaque, std::move(handler));
        if (bootstrapped_ && socket_.is_open()) {
            write_and_flush(data);
        } else {
            std::scoped_lock lock(pending_buffer_mutex_);
            pending_buffer_.push_back(data);
        }
    }

    void cancel(uint32_t opaque, std::error_code ec)
    {
        if (stopped_) {
            return;
        }
        auto handler = command_handlers_.find(opaque);
        if (handler != command_handlers_.end()) {
            spdlog::debug("{} MCBP cancel operation, opaque={}, ec={}", log_prefix_, opaque, ec.message());
            handler->second(ec, {});
            command_handlers_.erase(handler);
        }
    }

    [[nodiscard]] bool supports_feature(protocol::hello_feature feature)
    {
        return std::find(supported_features_.begin(), supported_features_.end(), feature) != supported_features_.end();
    }

    [[nodiscard]] std::vector<protocol::hello_feature> supported_features() const
    {
        return supported_features_;
    }

    [[nodiscard]] bool supports_gcccp()
    {
        return supports_gcccp_;
    }

    [[nodiscard]] bool has_config() const
    {
        return config_.has_value();
    }

    [[nodiscard]] configuration config() const
    {
        return config_.value();
    }

    [[nodiscard]] size_t index() const
    {
        Expects(config_.has_value());
        return config_->index_for_this_node();
    }

    [[nodiscard]] uint32_t next_opaque()
    {
        return ++opaque_;
    }

    std::error_code map_status_code(protocol::client_opcode opcode, uint16_t status)
    {
        switch (static_cast<protocol::status>(status)) {
            case protocol::status::success:
            case protocol::status::subdoc_multi_path_failure:
            case protocol::status::subdoc_success_deleted:
            case protocol::status::subdoc_multi_path_failure_deleted:
                return {};

            case protocol::status::not_found:
            case protocol::status::not_stored:
                return std::make_error_code(error::key_value_errc::document_not_found);

            case protocol::status::exists:
                if (opcode == protocol::client_opcode::insert) {
                    return std::make_error_code(error::key_value_errc::document_exists);
                }
                return std::make_error_code(error::common_errc::cas_mismatch);

            case protocol::status::too_big:
                return std::make_error_code(error::key_value_errc::value_too_large);

            case protocol::status::invalid:
            case protocol::status::xattr_invalid:
            case protocol::status::subdoc_invalid_combo:
                return std::make_error_code(error::common_errc::invalid_argument);

            case protocol::status::delta_bad_value:
                return std::make_error_code(error::key_value_errc::delta_invalid);

            case protocol::status::no_bucket:
                return std::make_error_code(error::common_errc::bucket_not_found);

            case protocol::status::locked:
                return std::make_error_code(error::key_value_errc::document_locked);

            case protocol::status::auth_stale:
            case protocol::status::auth_error:
            case protocol::status::no_access:
                return std::make_error_code(error::common_errc::authentication_failure);

            case protocol::status::not_supported:
            case protocol::status::unknown_command:
                return std::make_error_code(error::common_errc::unsupported_operation);

            case protocol::status::internal:
                return std::make_error_code(error::common_errc::internal_server_failure);

            case protocol::status::busy:
            case protocol::status::temp_failure:
            case protocol::status::no_memory:
            case protocol::status::not_initialized:
                return std::make_error_code(error::common_errc::temporary_failure);

            case protocol::status::unknown_collection:
                return std::make_error_code(error::common_errc::collection_not_found);

            case protocol::status::unknown_scope:
                return std::make_error_code(error::common_errc::scope_not_found);

            case protocol::status::durability_invalid_level:
                return std::make_error_code(error::key_value_errc::durability_level_not_available);

            case protocol::status::durability_impossible:
                return std::make_error_code(error::key_value_errc::durability_impossible);

            case protocol::status::sync_write_in_progress:
                return std::make_error_code(error::key_value_errc::durable_write_in_progress);

            case protocol::status::sync_write_ambiguous:
                return std::make_error_code(error::key_value_errc::durability_ambiguous);

            case protocol::status::sync_write_re_commit_in_progress:
                return std::make_error_code(error::key_value_errc::durable_write_re_commit_in_progress);

            case protocol::status::subdoc_path_not_found:
                return std::make_error_code(error::key_value_errc::path_not_found);

            case protocol::status::subdoc_path_mismatch:
                return std::make_error_code(error::key_value_errc::path_mismatch);

            case protocol::status::subdoc_path_invalid:
                return std::make_error_code(error::key_value_errc::path_invalid);

            case protocol::status::subdoc_path_too_big:
                return std::make_error_code(error::key_value_errc::path_too_big);

            case protocol::status::subdoc_doc_too_deep:
                return std::make_error_code(error::key_value_errc::value_too_deep);

            case protocol::status::subdoc_value_cannot_insert:
                return std::make_error_code(error::key_value_errc::value_invalid);

            case protocol::status::subdoc_doc_not_json:
                return std::make_error_code(error::key_value_errc::document_not_json);

            case protocol::status::subdoc_num_range_error:
                return std::make_error_code(error::key_value_errc::number_too_big);

            case protocol::status::subdoc_delta_invalid:
                return std::make_error_code(error::key_value_errc::delta_invalid);

            case protocol::status::subdoc_path_exists:
                return std::make_error_code(error::key_value_errc::path_exists);

            case protocol::status::subdoc_value_too_deep:
                return std::make_error_code(error::key_value_errc::value_too_deep);

            case protocol::status::subdoc_xattr_invalid_flag_combo:
            case protocol::status::subdoc_xattr_invalid_key_combo:
                return std::make_error_code(error::key_value_errc::xattr_invalid_key_combo);

            case protocol::status::subdoc_xattr_unknown_macro:
                return std::make_error_code(error::key_value_errc::xattr_unknown_macro);

            case protocol::status::subdoc_xattr_unknown_vattr:
                return std::make_error_code(error::key_value_errc::xattr_unknown_virtual_attribute);

            case protocol::status::subdoc_xattr_cannot_modify_vattr:
                return std::make_error_code(error::key_value_errc::xattr_cannot_modify_virtual_attribute);

            case protocol::status::subdoc_invalid_xattr_order:
            case protocol::status::not_my_vbucket:
            case protocol::status::auth_continue:
            case protocol::status::range_error:
            case protocol::status::rollback:
            case protocol::status::unknown_frame_info:
            case protocol::status::no_collections_manifest:
            case protocol::status::cannot_apply_collections_manifest:
            case protocol::status::collections_manifest_is_ahead:
            case protocol::status::dcp_stream_id_invalid:
                break;
        }
        // FIXME: use error map here
        spdlog::warn("{} unknown status code: {} (opcode={})", log_prefix_, status, opcode);
        return std::make_error_code(error::network_errc::protocol_error);
    }

    void update_configuration(configuration&& config)
    {
        if (stopped_) {
            return;
        }
        if (!config_ || config.rev > config_->rev) {
            for (auto& node : config.nodes) {
                if (node.this_node && node.hostname.empty()) {
                    node.hostname = endpoint_address_;
                }
            }
            config_.emplace(config);
            spdlog::debug("{} received new configuration: {}", log_prefix_, config_.value());
        }
    }

    std::optional<std::uint32_t> get_collection_uid(const std::string& collection_path)
    {
        return collection_cache_.get(collection_path);
    }

    void update_collection_uid(const std::string& path, std::uint32_t uid)
    {
        if (stopped_) {
            return;
        }
        collection_cache_.update(path, uid);
    }

  private:
    void invoke_bootstrap_handler(std::error_code ec)
    {
        if (!bootstrapped_ && bootstrap_handler_) {
            bootstrap_deadline_.cancel();
            bootstrap_handler_(ec, config_.value_or(configuration{}));
            bootstrap_handler_ = nullptr;
        }
        if (ec) {
            return stop();
        }
        bootstrapped_ = true;
        handler_ = std::make_unique<normal_handler>(shared_from_this());
        std::scoped_lock lock(pending_buffer_mutex_);
        if (!pending_buffer_.empty()) {
            for (const auto& buf : pending_buffer_) {
                write(buf);
            }
            pending_buffer_.clear();
            flush();
        }
    }

    void on_resolve(std::error_code ec, const asio::ip::tcp::resolver::results_type& endpoints)
    {
        if (stopped_) {
            return;
        }
        if (ec) {
            spdlog::error("{} error on resolve: {}", log_prefix_, ec.message());
            return initiate_bootstrap();
        }
        endpoints_ = endpoints;
        do_connect(endpoints_.begin());
        connection_deadline_.expires_after(timeout_defaults::connect_timeout);
        connection_deadline_.async_wait(std::bind(&mcbp_session::check_deadline, shared_from_this(), std::placeholders::_1));
    }

    void do_connect(asio::ip::tcp::resolver::results_type::iterator it)
    {
        if (stopped_) {
            return;
        }
        if (it != endpoints_.end()) {
            spdlog::debug("{} connecting to {}:{}", log_prefix_, it->endpoint().address().to_string(), it->endpoint().port());
            connection_deadline_.expires_after(timeout_defaults::connect_timeout);
            socket_.async_connect(it->endpoint(), std::bind(&mcbp_session::on_connect, shared_from_this(), std::placeholders::_1, it));
        } else {
            spdlog::error("{} no more endpoints left to connect, will try another address", log_prefix_);
            return initiate_bootstrap();
        }
    }

    void on_connect(const std::error_code& ec, asio::ip::tcp::resolver::results_type::iterator it)
    {
        if (stopped_) {
            return;
        }
        if (!socket_.is_open() || ec) {
            spdlog::warn(
              "{} unable to connect to {}:{}: {}", log_prefix_, it->endpoint().address().to_string(), it->endpoint().port(), ec.message());
            do_connect(++it);
        } else {
            socket_.set_option(asio::ip::tcp::no_delay{ true });
            socket_.set_option(asio::socket_base::keep_alive{ true });
            endpoint_ = it->endpoint();
            endpoint_address_ = endpoint_.address().to_string();
            spdlog::debug("{} connected to {}:{}", log_prefix_, endpoint_address_, it->endpoint().port());
            log_prefix_ =
              fmt::format("[{}/{}/{}] <{}:{}>", client_id_, id_, bucket_name_.value_or("-"), endpoint_address_, endpoint_.port());
            handler_ = std::make_unique<bootstrap_handler>(shared_from_this());
            connection_deadline_.expires_at(asio::steady_timer::time_point::max());
            connection_deadline_.cancel();
        }
    }

    void check_deadline(std::error_code ec)
    {
        if (ec == asio::error::operation_aborted || stopped_) {
            return;
        }
        if (connection_deadline_.expiry() <= asio::steady_timer::clock_type::now()) {
            socket_.close();
            connection_deadline_.expires_at(asio::steady_timer::time_point::max());
        }
        connection_deadline_.async_wait(std::bind(&mcbp_session::check_deadline, shared_from_this(), std::placeholders::_1));
    }

    void do_read()
    {
        if (stopped_) {
            return;
        }
        if (reading_) {
            return;
        }
        reading_ = true;
        socket_.async_read_some(
          asio::buffer(input_buffer_), [self = shared_from_this()](std::error_code ec, std::size_t bytes_transferred) {
              if (ec == asio::error::operation_aborted || self->stopped_) {
                  return;
              }
              if (ec) {
                  spdlog::error("{} IO error while reading from the socket: {}", self->log_prefix_, ec.message());
                  return self->stop();
              }
              self->parser_.feed(self->input_buffer_.data(), self->input_buffer_.data() + ssize_t(bytes_transferred));

              for (;;) {
                  mcbp_message msg{};
                  switch (self->parser_.next(msg)) {
                      case mcbp_parser::ok:
                          spdlog::debug(
                            "{} MCBP recv, opaque={}, {:n}", self->log_prefix_, msg.header.opaque, spdlog::to_hex(msg.header_data()));
                          SPDLOG_TRACE("{} MCBP recv, opaque={}{:a}{:a}",
                                       self->log_prefix_,
                                       msg.header.opaque,
                                       spdlog::to_hex(msg.header_data()),
                                       spdlog::to_hex(msg.body));
                          self->handler_->handle(std::move(msg));
                          if (self->stopped_) {
                              return;
                          }
                          break;
                      case mcbp_parser::need_data:
                          self->reading_ = false;
                          if (!self->stopped_) {
                              self->do_read();
                          }
                          return;
                      case mcbp_parser::failure:
                          return self->stop();
                  }
              }
          });
    }

    void do_write()
    {
        if (stopped_) {
            return;
        }
        std::scoped_lock lock(writing_buffer_mutex_, output_buffer_mutex_);
        if (!writing_buffer_.empty() || output_buffer_.empty()) {
            return;
        }
        std::swap(writing_buffer_, output_buffer_);
        std::vector<asio::const_buffer> buffers;
        buffers.reserve(writing_buffer_.size());
        for (auto& buf : writing_buffer_) {
            buffers.emplace_back(asio::buffer(buf));
        }
        asio::async_write(socket_, buffers, [self = shared_from_this()](std::error_code ec, std::size_t /*unused*/) {
            if (ec == asio::error::operation_aborted || self->stopped_) {
                return;
            }
            if (ec) {
                spdlog::error("{} IO error while writing to the socket: {}", self->log_prefix_, ec.message());
                return self->stop();
            }
            {
                std::scoped_lock inner_lock(self->writing_buffer_mutex_);
                self->writing_buffer_.clear();
            }
            self->do_write();
            self->do_read();
        });
    }

    std::string client_id_;
    std::string id_;
    asio::io_context& ctx_;
    asio::ip::tcp::resolver resolver_;
    asio::strand<asio::io_context::executor_type> strand_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer bootstrap_deadline_;
    asio::steady_timer connection_deadline_;
    asio::steady_timer retry_backoff_;
    couchbase::origin origin_;
    std::optional<std::string> bucket_name_;
    mcbp_parser parser_;
    std::unique_ptr<message_handler> handler_;
    std::function<void(std::error_code, const configuration&)> bootstrap_handler_;
    std::map<uint32_t, std::function<void(std::error_code, io::mcbp_message&&)>> command_handlers_{};

    bool bootstrapped_{ false };
    std::atomic_bool stopped_{ false };
    bool authenticated_{ false };
    bool bucket_selected_{ false };
    bool supports_gcccp_{ true };

    std::atomic<std::uint32_t> opaque_{ 0 };

    std::array<std::uint8_t, 16384> input_buffer_{};
    std::vector<std::vector<std::uint8_t>> output_buffer_{};
    std::vector<std::vector<std::uint8_t>> pending_buffer_{};
    std::vector<std::vector<std::uint8_t>> writing_buffer_{};
    std::mutex output_buffer_mutex_{};
    std::mutex pending_buffer_mutex_{};
    std::mutex writing_buffer_mutex_{};
    asio::ip::tcp::endpoint endpoint_{}; // connected endpoint
    std::string endpoint_address_{};     // cached string with endpoint address
    asio::ip::tcp::resolver::results_type endpoints_;
    std::vector<protocol::hello_feature> supported_features_;
    std::optional<configuration> config_;
    std::optional<error_map> errmap_;
    collection_cache collection_cache_;

    std::atomic_bool reading_{ false };

    std::string log_prefix_{};
};
} // namespace couchbase::io
