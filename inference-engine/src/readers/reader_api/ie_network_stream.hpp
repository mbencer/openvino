// Copyright (C) 2018-2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <ie_api.h>
#include <istream>
#include <fstream>
#include <string>

namespace InferenceEngine {
namespace details {

class INFERENCE_ENGINE_API_CLASS(NetworkStream): public std::istream {
public:
    NetworkStream(std::istream&& stream);

    NetworkStream(std::istream& stream);

    NetworkStream(std::istream& stream, const std::string& network_path);

    NetworkStream(std::istream&& stream, const std::string& network_path);

    bool isNetworkPathProvided() const;

    std::string getNetworkPath() const;

private:
    std::string _network_path;
};


}  // namespace details
}  // namespace InferenceEngine
