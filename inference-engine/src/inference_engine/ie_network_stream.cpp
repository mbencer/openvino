// Copyright (C) 2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "ie_network_stream.hpp"

InferenceEngine::details::NetworkStream::NetworkStream(std::istream&& stream)
    : std::istream(std::move(stream)) {
}

InferenceEngine::details::NetworkStream::NetworkStream(std::istream& stream)
    : std::istream(stream.rdbuf()) {
}

InferenceEngine::details::NetworkStream::NetworkStream(std::istream& stream, const std::string& network_path)
    : std::istream(stream.rdbuf()), _network_path{network_path} {
}

InferenceEngine::details::NetworkStream::NetworkStream(std::istream&& stream, const std::string& network_path)
    : std::istream(std::move(stream)), _network_path{network_path} {
}

bool InferenceEngine::details::NetworkStream::isNetworkPathProvided() const {
    return _network_path.empty();
}

std::string InferenceEngine::details::NetworkStream::getNetworkPath() const {
    return _network_path;
}
