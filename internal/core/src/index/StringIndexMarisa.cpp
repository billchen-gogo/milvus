// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License

#include "index/StringIndexMarisa.h"
#include "index/Utils.h"
#include "index/Index.h"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <knowhere/common/Utils.h>
#include <pb/schema.pb.h>

namespace milvus::scalar {

#ifdef __linux__

int64_t
StringIndexMarisa::Size() {
    return trie_.size();
}

void
StringIndexMarisa::Build(size_t n, const std::string* values) {
    if (built_) {
        throw std::runtime_error("index has been built");
    }

    marisa::Keyset keyset;
    {
        // fill key set.
        for (size_t i = 0; i < n; i++) {
            keyset.push_back(values[i].c_str());
        }
    }

    trie_.build(keyset);
    fill_str_ids(n, values);
    fill_offsets();

    built_ = true;
}

BinarySet
StringIndexMarisa::Serialize(const Config& config) {
    auto uuid = boost::uuids::random_generator()();
    auto uuid_string = boost::uuids::to_string(uuid);
    auto file = std::string("/tmp/") + uuid_string;

    auto fd = open(file.c_str(), O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IXUSR);
    trie_.write(fd);

    auto size = get_file_size(fd);
    auto buf = new uint8_t[size];

    while (read(fd, buf, size) != size) {
        lseek(fd, 0, SEEK_SET);
    }
    std::shared_ptr<uint8_t[]> index_data(buf);

    close(fd);
    remove(file.c_str());

    auto str_ids_len = str_ids_.size() * sizeof(size_t);
    std::shared_ptr<uint8_t[]> str_ids(new uint8_t[str_ids_len]);
    memcpy(str_ids.get(), str_ids_.data(), str_ids_len);

    BinarySet res_set;
    res_set.Append(MARISA_TRIE_INDEX, index_data, size);
    res_set.Append(MARISA_STR_IDS, str_ids, str_ids_len);

    knowhere::Disassemble(res_set, config);

    return res_set;
}

void
StringIndexMarisa::Load(const BinarySet& set) {
    knowhere::Assemble(const_cast<BinarySet&>(set));

    auto uuid = boost::uuids::random_generator()();
    auto uuid_string = boost::uuids::to_string(uuid);
    auto file = std::string("/tmp/") + uuid_string;

    auto index = set.GetByName(MARISA_TRIE_INDEX);
    auto len = index->size;

    auto fd = open(file.c_str(), O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IXUSR);
    lseek(fd, 0, SEEK_SET);
    while (write(fd, index->data.get(), len) != len) {
        lseek(fd, 0, SEEK_SET);
    }

    lseek(fd, 0, SEEK_SET);
    trie_.read(fd);
    close(fd);
    remove(file.c_str());

    auto str_ids = set.GetByName(MARISA_STR_IDS);
    auto str_ids_len = str_ids->size;
    str_ids_.resize(str_ids_len / sizeof(size_t));
    memcpy(str_ids_.data(), str_ids->data.get(), str_ids_len);

    fill_offsets();
}

const TargetBitmapPtr
StringIndexMarisa::In(size_t n, const std::string* values) {
    TargetBitmapPtr bitset = std::make_unique<TargetBitmap>(str_ids_.size());
    for (size_t i = 0; i < n; i++) {
        auto str = values[i];
        auto str_id = lookup(str);
        if (str_id >= 0) {
            auto offsets = str_ids_to_offsets_[str_id];
            for (auto offset : offsets) {
                bitset->set(offset);
            }
        }
    }
    return bitset;
}

const TargetBitmapPtr
StringIndexMarisa::NotIn(size_t n, const std::string* values) {
    TargetBitmapPtr bitset = std::make_unique<TargetBitmap>(str_ids_.size());
    bitset->set();
    for (size_t i = 0; i < n; i++) {
        auto str = values[i];
        auto str_id = lookup(str);
        if (str_id >= 0) {
            auto offsets = str_ids_to_offsets_[str_id];
            for (auto offset : offsets) {
                bitset->reset(offset);
            }
        }
    }
    return bitset;
}

const TargetBitmapPtr
StringIndexMarisa::Range(std::string value, OperatorType op) {
    throw std::runtime_error("todo: unsupported now");
}

const TargetBitmapPtr
StringIndexMarisa::Range(std::string lower_bound_value,
                         bool lb_inclusive,
                         std::string upper_bound_value,
                         bool ub_inclusive) {
    throw std::runtime_error("todo: unsupported now");
}

const TargetBitmapPtr
StringIndexMarisa::PrefixMatch(std::string prefix) {
    TargetBitmapPtr bitset = std::make_unique<TargetBitmap>(str_ids_.size());
    auto matched = prefix_match(prefix);
    for (const auto str_id : matched) {
        auto offsets = str_ids_to_offsets_[str_id];
        for (auto offset : offsets) {
            bitset->set(offset);
        }
    }
    return bitset;
}

void
StringIndexMarisa::fill_str_ids(size_t n, const std::string* values) {
    str_ids_.resize(n);
    for (size_t i = 0; i < n; i++) {
        auto str = values[i];
        auto str_id = lookup(str);
        assert(str_id >= 0);
        str_ids_[i] = str_id;
    }
}

void
StringIndexMarisa::fill_offsets() {
    for (size_t offset = 0; offset < str_ids_.size(); offset++) {
        auto str_id = str_ids_[offset];
        if (str_ids_to_offsets_.find(str_id) == str_ids_to_offsets_.end()) {
            str_ids_to_offsets_[str_id] = std::vector<size_t>{};
        }
        str_ids_to_offsets_[str_id].push_back(offset);
    }
}

size_t
StringIndexMarisa::lookup(const std::string& str) {
    marisa::Agent agent;
    agent.set_query(str.c_str());
    trie_.lookup(agent);
    return agent.key().id();
}

std::vector<size_t>
StringIndexMarisa::prefix_match(const std::string& prefix) {
    std::vector<size_t> ret;
    marisa::Agent agent;
    agent.set_query(prefix.c_str());
    while (trie_.predictive_search(agent)) {
        ret.push_back(agent.key().id());
    }
    return ret;
}

#endif

}  // namespace milvus::scalar
