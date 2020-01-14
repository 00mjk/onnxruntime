// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include "OnnxruntimeModel.h"
#include "core/platform/windows/TraceLoggingConfig.h"
#include <evntrace.h>

#include "OnnxruntimeDescriptorConverter.h"
#include "OnnxruntimeEngine.h"

using namespace Windows::AI::MachineLearning;

struct winml_adapter_api_model_feature_helper {
  decltype(WinmlAdapterApi::ModelGetInputCount) GetCount;
  decltype(WinmlAdapterApi::ModelGetInputName) GetName;
  decltype(WinmlAdapterApi::ModelGetInputDescription) GetDescription;
  decltype(WinmlAdapterApi::ModelGetInputTypeInfo) GetTypeInfo;
};

HRESULT CreateFeatureDescriptors(
    const winml_adapter_api_model_feature_helper* feature_helpers,
    OrtModel* ort_model,
    std::vector<OnnxruntimeValueInfoWrapper>& descriptors) {
  size_t count;
  if (auto status = feature_helpers->GetCount(ort_model, &count)) {
    return E_FAIL;
  }

  for (size_t i = 0; i < count; i++) {
    OnnxruntimeValueInfoWrapper descriptor;
    if (auto status = feature_helpers->GetName(ort_model, i, &descriptor.name_, &descriptor.name_length_)) {
      return E_FAIL;
    }
    if (auto status = feature_helpers->GetDescription(ort_model, i, &descriptor.description_, &descriptor.description_length_)) {
      return E_FAIL;
    }
    if (auto status = feature_helpers->GetTypeInfo(ort_model, i, &descriptor.type_info_)) {
      return E_FAIL;
    }

    descriptors.push_back(descriptor);
  }
  return S_OK;
}

HRESULT ModelInfo::RuntimeClassInitialize(OnnxruntimeEngineFactory* engine_factory, OrtModel* ort_model) {
  RETURN_HR_IF_NULL(E_INVALIDARG, ort_model);

  const auto winml_adapter_api = engine_factory->UseWinmlAdapterApi();

  // Get Metadata
  size_t count;
  if (auto status = winml_adapter_api->ModelGetMetadataCount(ort_model, &count)) {
    return E_FAIL;
  }

  const char* metadata_key;
  size_t metadata_key_len;
  const char* metadata_value;
  size_t metadata_value_len;
  for (size_t i = 0; i < count; i++) {
    if (auto status = winml_adapter_api->ModelGetMetadata(ort_model, i, &metadata_key, &metadata_key_len, &metadata_value, &metadata_value_len)) {
      return E_FAIL;
    }

    model_metadata_.insert_or_assign(
        std::string(metadata_key, metadata_key_len),
        std::string(metadata_value, metadata_value_len));
  }

  WinML::OnnxruntimeDescriptorConverter converter(engine_factory, model_metadata_);

  static const winml_adapter_api_model_feature_helper input_helpers = {
      winml_adapter_api->ModelGetInputCount,
      winml_adapter_api->ModelGetInputName,
      winml_adapter_api->ModelGetInputDescription,
      winml_adapter_api->ModelGetInputTypeInfo};

  // Create inputs
  std::vector<OnnxruntimeValueInfoWrapper> inputs;
  RETURN_IF_FAILED(CreateFeatureDescriptors(&input_helpers, ort_model, inputs));
  input_features_ = converter.ConvertToLearningModelDescriptors(inputs);

  // Create outputs
  static const winml_adapter_api_model_feature_helper output_helpers = {
      winml_adapter_api->ModelGetOutputCount,
      winml_adapter_api->ModelGetOutputName,
      winml_adapter_api->ModelGetOutputDescription,
      winml_adapter_api->ModelGetOutputTypeInfo};

  std::vector<OnnxruntimeValueInfoWrapper> outputs;
  RETURN_IF_FAILED(CreateFeatureDescriptors(&output_helpers, ort_model, outputs));
  output_features_ = converter.ConvertToLearningModelDescriptors(outputs);

  const char* out;
  size_t len;

  if (auto status = winml_adapter_api->ModelGetAuthor(ort_model, &out, &len)) {
    return E_FAIL;
  }
  author_ = std::string(out, len);

  if (auto status = winml_adapter_api->ModelGetName(ort_model, &out, &len)) {
    return E_FAIL;
  }
  name_ = std::string(out, len);

  if (auto status = winml_adapter_api->ModelGetDomain(ort_model, &out, &len)) {
    return E_FAIL;
  }
  domain_ = std::string(out, len);

  if (auto status = winml_adapter_api->ModelGetDescription(ort_model, &out, &len)) {
    return E_FAIL;
  }
  description_ = std::string(out, len);

  if (auto status = winml_adapter_api->ModelGetVersion(ort_model, &version_)) {
    return E_FAIL;
  }

  return S_OK;
}

STDMETHODIMP ModelInfo::GetAuthor(const char** out, size_t* len) {
  *out = author_.c_str();
  *len = author_.size();
  return S_OK;
}

STDMETHODIMP ModelInfo::GetName(const char** out, size_t* len) {
  *out = name_.c_str();
  *len = name_.size();
  return S_OK;
}

STDMETHODIMP ModelInfo::GetDomain(const char** out, size_t* len) {
  *out = domain_.c_str();
  *len = domain_.size();
  return S_OK;
}

STDMETHODIMP ModelInfo::GetDescription(const char** out, size_t* len) {
  *out = description_.c_str();
  *len = description_.size();
  return S_OK;
}

STDMETHODIMP ModelInfo::GetVersion(int64_t* out) {
  *out = version_;
  return S_OK;
}

STDMETHODIMP ModelInfo::GetModelMetadata(ABI::Windows::Foundation::Collections::IMapView<HSTRING, HSTRING>** metadata) {
  std::unordered_map<winrt::hstring, winrt::hstring> map_copy;
  for (auto& pair : model_metadata_) {
    auto metadata_key = WinML::Strings::HStringFromUTF8(pair.first);
    auto metadata_value = WinML::Strings::HStringFromUTF8(pair.second);
    map_copy.emplace(std::move(metadata_key), std::move(metadata_value));
  }
  auto map = winrt::single_threaded_map<winrt::hstring, winrt::hstring>(std::move(map_copy));
  winrt::copy_to_abi(map, *(void**)metadata);
  return S_OK;
}

STDMETHODIMP ModelInfo::GetInputFeatures(ABI::Windows::Foundation::Collections::IVectorView<winml::ILearningModelFeatureDescriptor>** features) {
  *features = nullptr;
  winrt::copy_to_abi(input_features_.GetView(), *(void**)features);
  return S_OK;
}

STDMETHODIMP ModelInfo::GetOutputFeatures(ABI::Windows::Foundation::Collections::IVectorView<winml::ILearningModelFeatureDescriptor>** features) {
  *features = nullptr;
  winrt::copy_to_abi(output_features_.GetView(), *(void**)features);
  return S_OK;
}

OnnruntimeModel::OnnruntimeModel() : ort_model_(nullptr, nullptr) {
}

STDMETHODIMP OnnruntimeModel::RuntimeClassInitialize(OnnxruntimeEngineFactory* engine_factory, UniqueOrtModel&& ort_model) {
  RETURN_HR_IF_NULL(E_INVALIDARG, ort_model);

  engine_factory_ = engine_factory;
  ort_model_ = std::move(ort_model);

  return S_OK;
}

STDMETHODIMP OnnruntimeModel::GetModelInfo(IModelInfo** info) {
  if (info_ == nullptr) {
    RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<ModelInfo>(&info_, engine_factory_.Get(), ort_model_.get()));
  }

  info_.CopyTo(info);

  return S_OK;
}

STDMETHODIMP OnnruntimeModel::ModelEnsureNoFloat16() {
  auto winml_adapter_api = engine_factory_->UseWinmlAdapterApi();
  if (auto status = winml_adapter_api->ModelEnsureNoFloat16(ort_model_.get())) {
    return E_FAIL;
  }
  return S_OK;
}

STDMETHODIMP OnnruntimeModel::CloneModel(IModel** copy) {
  auto winml_adapter_api = engine_factory_->UseWinmlAdapterApi();

  OrtModel* ort_model_copy;
  if (auto status = winml_adapter_api->CloneModel(ort_model_.get(), &ort_model_copy)) {
    return E_FAIL;
  }

  auto model = UniqueOrtModel(ort_model_copy, winml_adapter_api->ReleaseModel);
  RETURN_IF_FAILED(Microsoft::WRL::MakeAndInitialize<OnnruntimeModel>(copy, engine_factory_.Get(), std::move(model)));

  return S_OK;
}


STDMETHODIMP OnnruntimeModel::DetachOrtModel(OrtModel** model) {
  *model = ort_model_.release();
  return S_OK;
}
