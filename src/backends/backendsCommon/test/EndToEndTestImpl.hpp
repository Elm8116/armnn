//
// Copyright © 2017 Arm Ltd. All rights reserved.
// SPDX-License-Identifier: MIT
//
#pragma once

#include <ResolveType.hpp>

#include <armnn/ArmNN.hpp>
#include <armnn/INetwork.hpp>
#include <Profiling.hpp>

#include <backendsCommon/test/QuantizeHelper.hpp>

#include <boost/test/unit_test.hpp>

#include <vector>

namespace
{

using namespace armnn;

template<typename T>
bool ConstantUsageTest(const std::vector<BackendId>& computeDevice,
                       const TensorInfo& commonTensorInfo,
                       const std::vector<T>& inputData,
                       const std::vector<T>& constantData,
                       const std::vector<T>& expectedOutputData)
{
    // Create runtime in which test will run
    IRuntime::CreationOptions options;
    IRuntimePtr runtime(IRuntime::Create(options));

    // Builds up the structure of the network.
    INetworkPtr net(INetwork::Create());

    IConnectableLayer* input = net->AddInputLayer(0);
    IConnectableLayer* constant = net->AddConstantLayer(ConstTensor(commonTensorInfo, constantData));
    IConnectableLayer* add = net->AddAdditionLayer();
    IConnectableLayer* output = net->AddOutputLayer(0);

    input->GetOutputSlot(0).Connect(add->GetInputSlot(0));
    constant->GetOutputSlot(0).Connect(add->GetInputSlot(1));
    add->GetOutputSlot(0).Connect(output->GetInputSlot(0));

    // Sets the tensors in the network.
    input->GetOutputSlot(0).SetTensorInfo(commonTensorInfo);
    constant->GetOutputSlot(0).SetTensorInfo(commonTensorInfo);
    add->GetOutputSlot(0).SetTensorInfo(commonTensorInfo);

    // optimize the network
    IOptimizedNetworkPtr optNet = Optimize(*net, computeDevice, runtime->GetDeviceSpec());

    // Loads it into the runtime.
    NetworkId netId;
    runtime->LoadNetwork(netId, std::move(optNet));

    // Creates structures for input & output.
    std::vector<T> outputData(inputData.size());

    InputTensors inputTensors
    {
        {0, ConstTensor(runtime->GetInputTensorInfo(netId, 0), inputData.data())}
    };
    OutputTensors outputTensors
    {
        {0, Tensor(runtime->GetOutputTensorInfo(netId, 0), outputData.data())}
    };

    // Does the inference.
    runtime->EnqueueWorkload(netId, inputTensors, outputTensors);

    // Checks the results.
    return outputData == expectedOutputData;
}

inline bool ConstantUsageFloat32Test(const std::vector<BackendId>& backends)
{
    const TensorInfo commonTensorInfo({ 2, 3 }, DataType::Float32);

    return ConstantUsageTest(backends,
        commonTensorInfo,
        std::vector<float>{ 1.f, 2.f, 3.f, 4.f, 5.f, 6.f }, // Input.
        std::vector<float>{ 6.f, 5.f, 4.f, 3.f, 2.f, 1.f }, // Const input.
        std::vector<float>{ 7.f, 7.f, 7.f, 7.f, 7.f, 7.f }  // Expected output.
    );
}

inline bool ConstantUsageUint8Test(const std::vector<BackendId>& backends)
{
    TensorInfo commonTensorInfo({ 2, 3 }, DataType::QuantisedAsymm8);

    const float scale = 0.023529f;
    const int8_t offset = -43;

    commonTensorInfo.SetQuantizationScale(scale);
    commonTensorInfo.SetQuantizationOffset(offset);

    return ConstantUsageTest(backends,
        commonTensorInfo,
        QuantizedVector<uint8_t>(scale, offset, { 1.f, 2.f, 3.f, 4.f, 5.f, 6.f }), // Input.
        QuantizedVector<uint8_t>(scale, offset, { 6.f, 5.f, 4.f, 3.f, 2.f, 1.f }), // Const input.
        QuantizedVector<uint8_t>(scale, offset, { 7.f, 7.f, 7.f, 7.f, 7.f, 7.f })  // Expected output.
    );
}

template<typename T>
bool CompareBoolean(T a, T b)
{
    return (a == 0 && b == 0) ||(a != 0 && b != 0);
};

template<DataType ArmnnIType, DataType ArmnnOType,
         typename TInput = ResolveType<ArmnnIType>, typename TOutput = ResolveType<ArmnnOType>>
void EndToEndLayerTestImpl(INetworkPtr network,
                           const std::map<int, std::vector<TInput>>& inputTensorData,
                           const std::map<int, std::vector<TOutput>>& expectedOutputData,
                           std::vector<BackendId> backends)
{
    // Create runtime in which test will run
    IRuntime::CreationOptions options;
    IRuntimePtr runtime(IRuntime::Create(options));

    // optimize the network
    IOptimizedNetworkPtr optNet = Optimize(*network, backends, runtime->GetDeviceSpec());

    // Loads it into the runtime.
    NetworkId netId;
    runtime->LoadNetwork(netId, std::move(optNet));

    InputTensors inputTensors;
    inputTensors.reserve(inputTensorData.size());
    for (auto&& it : inputTensorData)
    {
        inputTensors.push_back({it.first,
                                ConstTensor(runtime->GetInputTensorInfo(netId, it.first), it.second.data())});
    }
    OutputTensors outputTensors;
    outputTensors.reserve(expectedOutputData.size());
    std::map<int, std::vector<TOutput>> outputStorage;
    for (auto&& it : expectedOutputData)
    {
        std::vector<TOutput> out(it.second.size());
        outputStorage.emplace(it.first, out);
        outputTensors.push_back({it.first,
                                 Tensor(runtime->GetOutputTensorInfo(netId, it.first),
                                               outputStorage.at(it.first).data())});
    }

    // Does the inference.
    runtime->EnqueueWorkload(netId, inputTensors, outputTensors);

    // Checks the results.
    for (auto&& it : expectedOutputData)
    {
        std::vector<TOutput> out = outputStorage.at(it.first);
        if (ArmnnOType == DataType::Boolean)
        {
            for (unsigned int i = 0; i < out.size(); ++i)
            {
                BOOST_TEST(CompareBoolean<TOutput>(it.second[i], out[i]));
            }
        }
        else
        {
            for (unsigned int i = 0; i < out.size(); ++i)
            {
                BOOST_TEST(it.second[i] == out[i], boost::test_tools::tolerance(0.000001f));
            }
        }
    }
}

inline void ImportNonAlignedInputPointerTest(std::vector<BackendId> backends)
{
    using namespace armnn;

    // Create runtime in which test will run
    IRuntime::CreationOptions options;
    IRuntimePtr runtime(armnn::IRuntime::Create(options));

    // build up the structure of the network
    INetworkPtr net(INetwork::Create());

    IConnectableLayer* input = net->AddInputLayer(0);

    NormalizationDescriptor descriptor;
    IConnectableLayer* norm = net->AddNormalizationLayer(descriptor);

    IConnectableLayer* output = net->AddOutputLayer(0);

    input->GetOutputSlot(0).Connect(norm->GetInputSlot(0));
    norm->GetOutputSlot(0).Connect(output->GetInputSlot(0));

    input->GetOutputSlot(0).SetTensorInfo(TensorInfo({ 1, 1, 4, 1 }, DataType::Float32));
    norm->GetOutputSlot(0).SetTensorInfo(TensorInfo({ 1, 1, 4, 1 }, DataType::Float32));

    // Optimize the network
    IOptimizedNetworkPtr optNet = Optimize(*net, backends, runtime->GetDeviceSpec());

    // Loads it into the runtime.
    NetworkId netId;
    std::string ignoredErrorMessage;
    // Enable Importing
    INetworkProperties networkProperties(true, true);
    runtime->LoadNetwork(netId, std::move(optNet), ignoredErrorMessage, networkProperties);

    // Creates structures for input & output
    std::vector<float> inputData
    {
        1.0f, 2.0f, 3.0f, 4.0f, 5.0f
    };

    // Misaligned input
    float* misalignedInputData = reinterpret_cast<float*>(reinterpret_cast<char*>(inputData.data()) + 1);

    std::vector<float> outputData(5);

    // Aligned output
    float * alignedOutputData = outputData.data();

    InputTensors inputTensors
    {
        {0,armnn::ConstTensor(runtime->GetInputTensorInfo(netId, 0), misalignedInputData)},
    };
    OutputTensors outputTensors
    {
        {0,armnn::Tensor(runtime->GetOutputTensorInfo(netId, 0), alignedOutputData)}
    };

    // The result of the inference is not important, just the fact that there
    // should not be CopyMemGeneric workloads.
    runtime->GetProfiler(netId)->EnableProfiling(true);

    // Do the inference and expect it to fail with a ImportMemoryException
    BOOST_CHECK_THROW(runtime->EnqueueWorkload(netId, inputTensors, outputTensors), MemoryImportException);
}

inline void ImportNonAlignedOutputPointerTest(std::vector<BackendId> backends)
{
    using namespace armnn;

    // Create runtime in which test will run
    IRuntime::CreationOptions options;
    IRuntimePtr runtime(armnn::IRuntime::Create(options));

    // build up the structure of the network
    INetworkPtr net(INetwork::Create());

    IConnectableLayer* input = net->AddInputLayer(0);

    NormalizationDescriptor descriptor;
    IConnectableLayer* norm = net->AddNormalizationLayer(descriptor);

    IConnectableLayer* output = net->AddOutputLayer(0);

    input->GetOutputSlot(0).Connect(norm->GetInputSlot(0));
    norm->GetOutputSlot(0).Connect(output->GetInputSlot(0));

    input->GetOutputSlot(0).SetTensorInfo(TensorInfo({ 1, 1, 4, 1 }, DataType::Float32));
    norm->GetOutputSlot(0).SetTensorInfo(TensorInfo({ 1, 1, 4, 1 }, DataType::Float32));

    // Optimize the network
    IOptimizedNetworkPtr optNet = Optimize(*net, backends, runtime->GetDeviceSpec());

    // Loads it into the runtime.
    NetworkId netId;
    std::string ignoredErrorMessage;
    // Enable Importing
    INetworkProperties networkProperties(true, true);
    runtime->LoadNetwork(netId, std::move(optNet), ignoredErrorMessage, networkProperties);

    // Creates structures for input & output
    std::vector<float> inputData
    {
        1.0f, 2.0f, 3.0f, 4.0f, 5.0f
    };

    // Aligned input
    float * alignedInputData = inputData.data();

    std::vector<float> outputData(5);

    // Misaligned output
    float* misalignedOutputData = reinterpret_cast<float*>(reinterpret_cast<char*>(outputData.data()) + 1);

    InputTensors inputTensors
    {
        {0,armnn::ConstTensor(runtime->GetInputTensorInfo(netId, 0), alignedInputData)},
    };
    OutputTensors outputTensors
    {
        {0,armnn::Tensor(runtime->GetOutputTensorInfo(netId, 0), misalignedOutputData)}
    };

    // The result of the inference is not important, just the fact that there
    // should not be CopyMemGeneric workloads.
    runtime->GetProfiler(netId)->EnableProfiling(true);

    // Do the inference and expect it to fail with a ImportMemoryException
    BOOST_CHECK_THROW(runtime->EnqueueWorkload(netId, inputTensors, outputTensors), MemoryExportException);
}

inline void ImportAlignedPointerTest(std::vector<BackendId> backends)
{
    using namespace armnn;

    // Create runtime in which test will run
    IRuntime::CreationOptions options;
    IRuntimePtr runtime(armnn::IRuntime::Create(options));

    // build up the structure of the network
    INetworkPtr net(INetwork::Create());

    IConnectableLayer* input = net->AddInputLayer(0);

    NormalizationDescriptor descriptor;
    IConnectableLayer* norm = net->AddNormalizationLayer(descriptor);

    IConnectableLayer* output = net->AddOutputLayer(0);

    input->GetOutputSlot(0).Connect(norm->GetInputSlot(0));
    norm->GetOutputSlot(0).Connect(output->GetInputSlot(0));

    input->GetOutputSlot(0).SetTensorInfo(TensorInfo({ 1, 1, 4, 1 }, DataType::Float32));
    norm->GetOutputSlot(0).SetTensorInfo(TensorInfo({ 1, 1, 4, 1 }, DataType::Float32));

    // Optimize the network
    IOptimizedNetworkPtr optNet = Optimize(*net, backends, runtime->GetDeviceSpec());

    // Loads it into the runtime.
    NetworkId netId;
    std::string ignoredErrorMessage;
    // Enable Importing
    INetworkProperties networkProperties(true, true);
    runtime->LoadNetwork(netId, std::move(optNet), ignoredErrorMessage, networkProperties);

    // Creates structures for input & output
    std::vector<float> inputData
    {
        1.0f, 2.0f, 3.0f, 4.0f
    };

    std::vector<float> outputData(4);

    InputTensors inputTensors
    {
        {0,armnn::ConstTensor(runtime->GetInputTensorInfo(netId, 0), inputData.data())},
    };
    OutputTensors outputTensors
    {
        {0,armnn::Tensor(runtime->GetOutputTensorInfo(netId, 0), outputData.data())}
    };

    // The result of the inference is not important, just the fact that there
    // should not be CopyMemGeneric workloads.
    runtime->GetProfiler(netId)->EnableProfiling(true);

    // Do the inference
    runtime->EnqueueWorkload(netId, inputTensors, outputTensors);

    // Retrieve the Profiler.Print() output to get the workload execution
    ProfilerManager& profilerManager = armnn::ProfilerManager::GetInstance();
    std::stringstream ss;
    profilerManager.GetProfiler()->Print(ss);;
    std::string dump = ss.str();

    // Contains RefNormalizationWorkload
    std::size_t found = dump.find("RefNormalizationWorkload");
    BOOST_TEST(found != std::string::npos);
    // Contains SyncMemGeneric
    found = dump.find("SyncMemGeneric");
    BOOST_TEST(found != std::string::npos);
    // No contains CopyMemGeneric
    found = dump.find("CopyMemGeneric");
    BOOST_TEST(found == std::string::npos);
}

} // anonymous namespace
