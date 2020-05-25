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

#include <protocol/unsigned_leb128.h>

#include <protocol/client_opcode.hxx>
#include <operations/document_id.hxx>

namespace couchbase::protocol
{

class mutate_in_response_body
{
  public:
    static const inline client_opcode opcode = client_opcode::subdoc_multi_mutation;

    struct mutate_in_field {
        std::uint8_t index;
        protocol::status status;
        std::string value;
    };

  private:
    std::vector<mutate_in_field> fields_;

  public:
    std::vector<mutate_in_field>& fields()
    {
        return fields_;
    }

    bool parse(protocol::status status, const header_buffer& header, const std::vector<uint8_t>& body, const cmd_info&)
    {
        Expects(header[1] == static_cast<uint8_t>(opcode));
        if (status == protocol::status::success || status == protocol::status::subdoc_multi_path_failure) {
            using offset_type = std::vector<uint8_t>::difference_type;
            uint8_t ext_size = header[4];
            offset_type offset = ext_size;
            fields_.reserve(16); /* we won't have more than 16 entries anyway */
            while (static_cast<std::size_t>(offset) < body.size()) {
                mutate_in_field field;

                field.index = body[static_cast<std::size_t>(offset)];
                Expects(field.index < 16);
                offset++;

                std::uint16_t entry_status = 0;
                memcpy(&entry_status, body.data() + offset, sizeof(entry_status));
                entry_status = ntohs(entry_status);
                Expects(is_valid_status(entry_status));
                field.status = static_cast<protocol::status>(entry_status);
                offset += static_cast<offset_type>(sizeof(entry_status));

                if (field.status == protocol::status::success) {
                    std::uint32_t entry_size = 0;
                    memcpy(&entry_size, body.data() + offset, sizeof(entry_size));
                    entry_size = ntohl(entry_size);
                    Expects(entry_size < 20 * 1024 * 1024);
                    offset += static_cast<offset_type>(sizeof(entry_size));

                    field.value.resize(entry_size);
                    memcpy(field.value.data(), body.data() + offset, entry_size);
                    offset += static_cast<offset_type>(entry_size);
                }

                fields_.emplace_back(field);
            }
            return true;
        }
        return false;
    }
};

class mutate_in_request_body
{
  public:
    using response_body_type = mutate_in_response_body;
    static const inline client_opcode opcode = client_opcode::subdoc_multi_mutation;

    static const inline uint8_t doc_flag_access_deleted = 0x04;

    struct mutate_in_specs {
        static const inline uint8_t path_flag_create_parents = 0x01;
        static const inline uint8_t path_flag_xattr = 0x04;
        static const inline uint8_t path_flag_expand_macros = 0x10;

        struct entry {
            std::uint8_t opcode;
            std::uint8_t flags;
            std::string path;
            std::string param;
        };
        std::vector<entry> entries;

        static inline uint8_t build_path_flags(bool xattr, bool create_parents, bool expand_macros)
        {
            uint8_t flags = 0;
            if (xattr) {
                flags |= path_flag_xattr;
            }
            if (create_parents) {
                flags |= path_flag_create_parents;
            }
            if (expand_macros) {
                flags |= path_flag_expand_macros;
            }
            return flags;
        }

        void add_spec(subdoc_opcode operation,
                      bool xattr,
                      bool create_parents,
                      bool expand_macros,
                      const std::string& path,
                      const std::string& param)
        {
            add_spec(static_cast<std::uint8_t>(operation), build_path_flags(xattr, create_parents, expand_macros), path, param);
        }

        void add_spec(subdoc_opcode operation,
                      bool xattr,
                      bool create_parents,
                      bool expand_macros,
                      const std::string& path,
                      const std::int64_t increment)
        {
            Expects(operation == protocol::subdoc_opcode::counter);
            add_spec(static_cast<std::uint8_t>(operation),
                     build_path_flags(xattr, create_parents, expand_macros),
                     path,
                     std::to_string(increment));
        }

        void add_spec(subdoc_opcode operation, bool xattr, const std::string& path)
        {
            Expects(operation == protocol::subdoc_opcode::remove);
            add_spec(static_cast<std::uint8_t>(operation), build_path_flags(xattr, false, false), path, "");
        }

        void add_spec(uint8_t operation, uint8_t flags, const std::string& path, const std::string& param)
        {
            Expects(is_valid_subdoc_opcode(operation));
            entries.emplace_back(entry{ operation, flags, path, param });
        }
    };

  private:
    std::string key_;
    std::vector<std::uint8_t> ext_{};
    std::vector<std::uint8_t> value_{};

    std::uint8_t flags_{ 0 };
    mutate_in_specs specs_;

  public:
    void id(const operations::document_id& id)
    {
        key_ = id.key;
    }

    void access_deleted(bool value)
    {
        if (value) {
            flags_ = doc_flag_access_deleted;
        } else {
            flags_ = 0;
        }
    }

    void specs(const mutate_in_specs& specs)
    {
        specs_ = specs;
    }

    const std::string& key()
    {
        return key_;
    }

    const std::vector<std::uint8_t>& extension()
    {
        if (ext_.empty()) {
            fill_extention();
        }
        return ext_;
    }

    const std::vector<std::uint8_t>& value()
    {
        if (value_.empty()) {
            fill_value();
        }
        return value_;
    }

    std::size_t size()
    {
        if (ext_.empty()) {
            fill_extention();
        }
        if (value_.empty()) {
            fill_value();
        }
        return key_.size() + ext_.size() + value_.size();
    }

  private:
    void fill_extention()
    {
        if (flags_ != 0) {
            ext_.resize(sizeof(flags_));
            ext_[0] = flags_;
        }
    }

    void fill_value()
    {
        size_t value_size = 0;
        for (auto& spec : specs_.entries) {
            value_size += sizeof(spec.opcode) + sizeof(spec.flags) + sizeof(std::uint16_t) + spec.path.size() + sizeof(std::uint32_t) +
                          spec.param.size();
        }
        Expects(value_size > 0);
        value_.resize(value_size);
        std::vector<std::uint8_t>::size_type offset = 0;
        for (auto& spec : specs_.entries) {
            value_[offset++] = spec.opcode;
            value_[offset++] = spec.flags;

            std::uint16_t path_size = ntohs(gsl::narrow_cast<std::uint16_t>(spec.path.size()));
            std::memcpy(value_.data() + offset, &path_size, sizeof(path_size));
            offset += sizeof(path_size);

            std::uint32_t param_size = ntohl(gsl::narrow_cast<std::uint32_t>(spec.param.size()));
            std::memcpy(value_.data() + offset, &param_size, sizeof(param_size));
            offset += sizeof(param_size);

            std::memcpy(value_.data() + offset, spec.path.data(), spec.path.size());
            offset += spec.path.size();

            if (param_size != 0u) {
                std::memcpy(value_.data() + offset, spec.param.data(), spec.param.size());
                offset += spec.param.size();
            }
        }
    }
};

} // namespace couchbase::protocol