// Copyright 2021-2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <unistd.h>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include "grpc_client.h"
#include "http_client.h"

namespace tc = triton::client;

#define FAIL_IF_ERR(X, MSG)                                        \
  {                                                                \
    tc::Error err = (X);                                           \
    if (!err.IsOk()) {                                             \
      std::cerr << "error: " << (MSG) << ": " << err << std::endl; \
      exit(1);                                                     \
    }                                                              \
  }

#define INPUT_DIM 16
#define INT32_BYTE_SIZE 4

namespace {

void
ValidateShapeAndDatatype(
    const std::string& name, const std::shared_ptr<tc::InferResult> result)
{
  std::vector<int64_t> shape;
  FAIL_IF_ERR(
      result->Shape(name, &shape), "unable to get shape for '" + name + "'");
  // Validate shape
  if ((shape.size() != 2) || (shape[0] != 1) || (shape[1] != INPUT_DIM)) {
    std::cerr << "error: received incorrect shapes for '" << name << "'"
              << std::endl;
    exit(1);
  }
  std::string datatype;
  FAIL_IF_ERR(
      result->Datatype(name, &datatype),
      "unable to get datatype for '" + name + "'");
  // Validate datatype
  if (datatype.compare("INT32") != 0) {
    std::cerr << "error: received incorrect datatype for '" << name
              << "': " << datatype << std::endl;
    exit(1);
  }
}

void
ValidateResult(
    const std::shared_ptr<tc::InferResult> result,
    const std::vector<int32_t>& input0_data)
{
  // Validate the results...
  ValidateShapeAndDatatype("OUTPUT0", result);

  // Get pointers to the result returned...
  int32_t* output0_data;
  size_t output0_byte_size;
  FAIL_IF_ERR(
      result->RawData(
          "OUTPUT0", (const uint8_t**)&output0_data, &output0_byte_size),
      "unable to get result data for 'OUTPUT0'");
  if (output0_byte_size != INPUT_DIM * INT32_BYTE_SIZE) {
    std::cerr << "error: received incorrect byte size for 'OUTPUT0': "
              << output0_byte_size << std::endl;
    exit(1);
  }

  for (size_t i = 0; i < INPUT_DIM; ++i) {
    if ((input0_data[i]) != *(output0_data + i)) {
      std::cerr << "error: incorrect output" << std::endl;
      exit(1);
    }
  }

  // Get full response
  std::cout << result->DebugString() << std::endl;
}

void
ValidateResponse(
    const std::shared_ptr<tc::InferResult> results_ptr,
    const std::vector<int32_t>& input0_data)
{
  // Validate results
  if (results_ptr->RequestStatus().IsOk()) {
    ValidateResult(results_ptr, input0_data);
  } else {
    std::cerr << "error: Inference failed: " << results_ptr->RequestStatus()
              << std::endl;
    exit(1);
  }
}

template <typename Client>
void
InferWithRetries(
    const std::unique_ptr<Client>& client, tc::InferResult** results,
    tc::InferOptions& options, std::vector<tc::InferInput*>& inputs,
    std::vector<const tc::InferRequestedOutput*>& outputs)
{
  auto err = client->Infer(results, options, inputs, outputs);

  // If the host runs out of available sockets due to TIME_WAIT, sleep and
  // retry on failure to give time for sockets to become available.
  int max_retries = 5;
  int sleep_secs = 60;
  for (int i = 0; !err.IsOk() && i < max_retries; i++) {
    std::cerr << "Error: " << err << std::endl;
    std::cerr << "Sleeping for " << sleep_secs
              << " seconds and retrying. [Attempt: " << i + 1 << "/"
              << max_retries << "]" << std::endl;
    sleep(sleep_secs);

    // Retry and break from loop on success
    err = client->Infer(results, options, inputs, outputs);
  }

  if (!err.IsOk()) {
    std::cerr << "error: Exceeded max tries [" << max_retries
              << "] on inference without success" << std::endl;
    exit(1);
  }
}

// Client should be tc::InferenceServerHttpClient or
// tc::InferenceServerGrpcClient
template <typename Client>
void
RunSyncInfer(
    std::vector<tc::InferInput*>& inputs,
    std::vector<const tc::InferRequestedOutput*>& outputs,
    tc::InferOptions& options, std::vector<int32_t>& input0_data, bool reuse,
    std::string url, bool verbose, uint32_t repetitions)
{
  // If re-use is enabled then use these client objects else use new objects for
  // each inference request.
  std::unique_ptr<Client> client;
  FAIL_IF_ERR(Client::Create(&client, url, verbose), "unable to create client");

  for (size_t i = 0; i < repetitions; ++i) {
    if (!reuse) {
      // Create new client connection on every request if reuse flag not set
      FAIL_IF_ERR(
          Client::Create(&client, url, verbose), "unable to create client");
    }

    tc::InferResult* results;
    InferWithRetries<Client>(client, &results, options, inputs, outputs);
    std::shared_ptr<tc::InferResult> results_ptr(results);
    ValidateResponse(results_ptr, input0_data);
  }
}

void
Usage(char** argv, const std::string& msg = std::string())
{
  if (!msg.empty()) {
    std::cerr << "error: " << msg << std::endl;
  }

  std::cerr << "Usage: " << argv[0] << " [options]" << std::endl;
  std::cerr << "\t-v" << std::endl;
  std::cerr << "\t-i <http/grpc>" << std::endl;
  std::cerr << "\t-u <URL for inference service>" << std::endl;
  std::cerr << "\t-t <client timeout in microseconds>" << std::endl;
  std::cerr << "\t-r <number of repetitions for inference> default is 100."
            << std::endl;
  std::cerr
      << "\t-R Re-use the same client for each repetition. Without "
         "this flag, the default is to create a new client on each repetition."
      << std::endl;
  std::cerr << std::endl;

  exit(1);
}

}  // namespace

int
main(int argc, char** argv)
{
  bool verbose = false;
  std::string protocol = "http";
  std::string url;
  bool reuse = false;
  uint32_t repetitions = 100;

  // Parse commandline...
  int opt;
  while ((opt = getopt(argc, argv, "vi:u:r:R")) != -1) {
    switch (opt) {
      case 'v':
        verbose = true;
        break;
      case 'i': {
        std::string p(optarg);
        std::transform(p.begin(), p.end(), p.begin(), ::tolower);
        if (p == "grpc" || p == "http") {
          protocol = p;
        } else {
          protocol = "unknown";
        }
        break;
      }
      case 'u':
        url = optarg;
        break;
      case 'r':
        repetitions = std::stoi(optarg);
        break;
      case 'R':
        reuse = true;
        break;
      case '?':
        Usage(argv);
        break;
    }
  }

  // Option validations
  if (protocol == "unknown") {
    std::cerr << "Supports only http and grpc protocols" << std::endl;
    Usage(argv);
  }

  std::string model_name = "custom_identity_int32";
  std::string model_version = "";

  if (protocol == "grpc") {
    if (url.empty()) {
      url = "localhost:8001";
    }
  } else {
    if (url.empty()) {
      url = "localhost:8000";
    }
    protocol = "http";
  }

  // Initialize the tensor data
  std::vector<int32_t> input0_data(INPUT_DIM);
  for (size_t i = 0; i < INPUT_DIM; ++i) {
    input0_data[i] = i;
  }

  std::vector<int64_t> shape{1, INPUT_DIM};

  // Initialize the inputs with the data.
  tc::InferInput* input0;

  FAIL_IF_ERR(
      tc::InferInput::Create(&input0, "INPUT0", shape, "INT32"),
      "unable to get INPUT0");
  std::shared_ptr<tc::InferInput> input0_ptr;
  input0_ptr.reset(input0);

  FAIL_IF_ERR(
      input0_ptr->AppendRaw(
          reinterpret_cast<uint8_t*>(&input0_data[0]),
          input0_data.size() * sizeof(int32_t)),
      "unable to set data for INPUT0");

  // Generate the outputs to be requested.
  tc::InferRequestedOutput* output0;
  FAIL_IF_ERR(
      tc::InferRequestedOutput::Create(&output0, "OUTPUT0"),
      "unable to get 'OUTPUT0'");
  std::shared_ptr<tc::InferRequestedOutput> output0_ptr;
  output0_ptr.reset(output0);

  // The inference settings. Will be using default for now.
  tc::InferOptions options(model_name);
  options.model_version_ = model_version;

  std::vector<tc::InferInput*> inputs = {input0_ptr.get()};
  std::vector<const tc::InferRequestedOutput*> outputs = {output0_ptr.get()};

  // Send 'repetitions' number of inference requests to the inference server.
  if (protocol == "http") {
    RunSyncInfer<tc::InferenceServerHttpClient>(
        inputs, outputs, options, input0_data, reuse, url, verbose,
        repetitions);
  } else if (protocol == "grpc") {
    RunSyncInfer<tc::InferenceServerGrpcClient>(
        inputs, outputs, options, input0_data, reuse, url, verbose,
        repetitions);
  } else {
    std::cerr << "Invalid protocol: " << protocol << std::endl;
    return 1;
  }

  return 0;
}
