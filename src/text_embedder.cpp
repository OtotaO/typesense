#include "text_embedder.h"
#include "text_embedder_manager.h"
#include "logger.h"
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>

TextEmbedder::TextEmbedder(const std::string& model_path) {
    // create environment
    Ort::SessionOptions session_options;
    std::string abs_path = TextEmbedderManager::get_absolute_model_path(model_path);
    LOG(INFO) << "Loading model from: " << abs_path;
    session_ = std::make_unique<Ort::Session>(env_, abs_path.c_str(), session_options);
    std::ifstream stream(TextEmbedderManager::get_absolute_vocab_path());
    std::stringstream ss;
    ss << stream.rdbuf();
    auto vocab_ = ss.str();
    tokenizer_ = std::make_unique<BertTokenizer>(vocab_, true, true, ustring("[UNK]"), ustring("[SEP]"), ustring("[PAD]"),
                    ustring("[CLS]"), ustring("[MASK]"), true, true, ustring("##"),512, std::string("longest_first"));
    auto output_tensor_count = session_->GetOutputCount();
    for (size_t i = 0; i < output_tensor_count; i++) {
        auto shape = session_->GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() == 3 && shape[0] == -1 && shape[1] == -1 && shape[2] > 0) {
            Ort::AllocatorWithDefaultOptions allocator;
            output_tensor_name = std::string(session_->GetOutputNameAllocated(i, allocator).get());
            break;
        }
    }
}

TextEmbedder::TextEmbedder(const std::string& openai_model_path, const std::string& openai_api_key) : openai_api_key(openai_api_key), openai_model_path(openai_model_path) {

}


encoded_input_t TextEmbedder::Encode(const std::string& text) {

    auto encoded = tokenizer_->Encode(tokenizer_->Tokenize(ustring(text)));
    auto input_ids = tokenizer_->AddSpecialToken(encoded);
    auto token_type_ids = tokenizer_->GenerateTypeId(encoded);
    auto attention_mask = std::vector<int64_t>(input_ids.size(), 1);
    // BERT supports max sequence length of 512
    if (input_ids.size() > 512) {
        input_ids.resize(512);
        token_type_ids.resize(512);
        attention_mask.resize(512);
    }
    return {input_ids, token_type_ids, attention_mask};
}


std::vector<float> TextEmbedder::mean_pooling(const std::vector<std::vector<float>>& inputs) {

    std::vector<float> pooled_output;
    for (int i = 0; i < inputs[0].size(); i++) {
        float sum = 0;
        for (int j = 0; j < inputs.size(); j++) {
            sum += inputs[j][i];
        }
        pooled_output.push_back(sum / inputs.size());
    }
    return pooled_output;
}

Option<std::vector<float>> TextEmbedder::Embed(const std::string& text) {
    if(is_openai()) {
        HttpClient& client = HttpClient::get_instance();
        std::unordered_map<std::string, std::string> headers;
        std::map<std::string, std::string> res_headers;
        headers["Authorization"] = "Bearer " + openai_api_key;
        headers["Content-Type"] = "application/json";
        std::string res;
            nlohmann::json req_body;
        req_body["input"] = text;
        // remove "openai/" prefix
        req_body["model"] = openai_model_path.substr(7);
        auto res_code = client.post_response(TextEmbedder::OPENAI_CREATE_EMBEDDING, req_body.dump(), res, res_headers, headers);
        if (res_code != 200) {
            LOG(ERROR) << "OpenAI API error: " << res;
            return Option<std::vector<float>>(400, "OpenAI API error: " + res);
        }
        return Option<std::vector<float>>(nlohmann::json::parse(res)["data"][0]["embedding"].get<std::vector<float>>());
    } else {
        auto encoded_input = Encode(text);
        // create input tensor object from data values
        Ort::AllocatorWithDefaultOptions allocator;
        Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);
        std::vector<Ort::Value> input_tensors;
        std::vector<std::vector<int64_t>> input_shapes;
        std::vector<const char*> input_node_names = {"input_ids", "attention_mask", "token_type_ids"};
        input_shapes.push_back({1, static_cast<int64_t>(encoded_input.input_ids.size())});
        input_shapes.push_back({1, static_cast<int64_t>(encoded_input.attention_mask.size())});
        input_shapes.push_back({1, static_cast<int64_t>(encoded_input.token_type_ids.size())});
        input_tensors.push_back(Ort::Value::CreateTensor<int64_t>(memory_info, encoded_input.input_ids.data(), encoded_input.input_ids.size(), input_shapes[0].data(), input_shapes[0].size()));
        input_tensors.push_back(Ort::Value::CreateTensor<int64_t>(memory_info, encoded_input.attention_mask.data(), encoded_input.attention_mask.size(), input_shapes[1].data(), input_shapes[1].size()));
        input_tensors.push_back(Ort::Value::CreateTensor<int64_t>(memory_info, encoded_input.token_type_ids.data(), encoded_input.token_type_ids.size(), input_shapes[2].data(), input_shapes[2].size()));

        //LOG(INFO) << "Running model";
        // create output tensor object
        std::vector<const char*> output_node_names = {output_tensor_name.c_str()};
        std::vector<int64_t> output_node_dims {1, static_cast<int64_t>(encoded_input.input_ids.size()), 384}; // batch_size x seq_length x hidden_size
        auto output_tensor = session_->Run(Ort::RunOptions{nullptr}, input_node_names.data(), input_tensors.data(), input_tensors.size(), output_node_names.data(), output_node_names.size());
        std::vector<std::vector<float>> output;
        float* data = output_tensor[0].GetTensorMutableData<float>();
        // print output tensor shape
        auto shape = output_tensor[0].GetTensorTypeAndShapeInfo().GetShape();

        for (int i = 0; i < shape[1]; i++) {
            std::vector<float> temp;
            for (int j = 0; j < shape[2]; j++) {
                temp.push_back(data[i * shape[2] + j]);
            }
            output.push_back(temp);
        }
        auto pooled_output = mean_pooling(output);  

        return Option<std::vector<float>>(pooled_output);
    }
}

Option<std::vector<std::vector<float>>> TextEmbedder::batch_embed(const std::vector<std::string>& inputs) {
    std::vector<std::vector<float>> outputs;
    if(!is_openai()) {
        // for now only openai is supported for batch embedding
        for(const auto& input : inputs) {
            outputs.push_back(Embed(input).get());
        }
    } else {
        nlohmann::json req_body;
        req_body["input"] = inputs;
        // remove "openai/" prefix
        req_body["model"] = openai_model_path.substr(7);
        std::unordered_map<std::string, std::string> headers;
        headers["Authorization"] = "Bearer " + openai_api_key;
        headers["Content-Type"] = "application/json";
        std::map<std::string, std::string> res_headers;
        std::string res;
        HttpClient& client = HttpClient::get_instance();

        auto res_code = client.post_response(OPENAI_CREATE_EMBEDDING, req_body.dump(), res, res_headers, headers);

        if(res_code != 200) {
            LOG(ERROR) << "OpenAI API error: " << res;
            return Option<std::vector<std::vector<float>>>(400, res);
        }

        nlohmann::json res_json = nlohmann::json::parse(res);
        for(auto& data : res_json["data"]) {
            outputs.push_back(data["embedding"].get<std::vector<float>>());
        }
    }
    return Option<std::vector<std::vector<float>>>(outputs);
}

TextEmbedder::~TextEmbedder() {
}


bool TextEmbedder::is_model_valid(const std::string& model_path, unsigned int& num_dims) {
   LOG(INFO) << "Loading model: " << model_path;
    Ort::SessionOptions session_options;
    Ort::Env env;
    std::string abs_path = TextEmbedderManager::get_absolute_model_path(model_path);

    if(!std::filesystem::exists(abs_path)) {
        LOG(ERROR) << "Model file not found: " << abs_path;
        return false;
    }

    Ort::Session session(env, abs_path.c_str(), session_options);
    if(session.GetInputCount() != 3) {
        LOG(ERROR) << "Invalid model: input count is not 3";
        return false;
    }
    Ort::AllocatorWithDefaultOptions allocator;
    auto input_ids_name = session.GetInputNameAllocated(0, allocator);
    if (std::strcmp(input_ids_name.get(), "input_ids") != 0) {
        LOG(ERROR) << "Invalid model: input_ids tensor not found";
        return false;
    }

    auto attention_mask_name = session.GetInputNameAllocated(1, allocator);
    if (std::strcmp(attention_mask_name.get(), "attention_mask") != 0) {
        LOG(ERROR) << "Invalid model: attention_mask tensor not found";
        return false;
    }

    auto token_type_ids_name = session.GetInputNameAllocated(2, allocator);
    if (std::strcmp(token_type_ids_name.get(), "token_type_ids") != 0) {
        LOG(ERROR) << "Invalid model: token_type_ids tensor not found";
        return false;
    }

    auto output_tensor_count = session.GetOutputCount();
    bool found_output_tensor = false;
    for (size_t i = 0; i < output_tensor_count; i++) {
        auto shape = session.GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() == 3 && shape[0] == -1 && shape[1] == -1 && shape[2] > 0) {
            num_dims = shape[2];
            found_output_tensor = true;
            break;
        }
    }

    if (!found_output_tensor) {
        LOG(ERROR) << "Invalid model: Output tensor not found";
        return false;
    }

    return true;
}


Option<bool> TextEmbedder::is_model_valid(const std::string openai_model_path, const std::string openai_api_key, unsigned int& num_dims) {
    if (openai_model_path.empty() || openai_api_key.empty() || openai_model_path.length() < 7) {
        return Option<bool>(400, "Invalid OpenAI model path or API key");
    }

    HttpClient& client = HttpClient::get_instance();
    std::unordered_map<std::string, std::string> headers;
    std::map<std::string, std::string> res_headers;
    headers["Authorization"] = "Bearer " + openai_api_key;
    std::string res;
    auto res_code = client.get_response(TextEmbedder::OPENAI_LIST_MODELS, res, res_headers, headers);
    if (res_code != 200) {
        LOG(ERROR) << "OpenAI API error: " << res;
        return Option<bool>(400, "OpenAI API error: " + res);
    }

    auto models_json = nlohmann::json::parse(res);
    bool found = false;
    // extract model name by removing "openai/" prefix
    auto model_name = openai_model_path.substr(7);
    for (auto& model : models_json["data"]) {
        if (model["id"] == model_name) {
            found = true;
            break;
        }
    }

    if (!found) {
        return Option<bool>(400, "OpenAI model not found");
    }


    // This part is hard coded for now. Because OpenAI API does not provide a way to get the output dimensions of the model.
    if(model_name.find("-ada-") != std::string::npos) {
        if(model_name.substr(model_name.length() - 3) == "002") {
            num_dims = 1536;
        } else {
            num_dims = 1024;
        }
    }
    else if(model_name.find("-davinci-") != std::string::npos) {
        num_dims = 12288;
    } else if(model_name.find("-curie-") != std::string::npos) {
        num_dims = 4096;
    } else if(model_name.find("-babbage-") != std::string::npos) {
        num_dims = 2048;
    } else {
        num_dims = 768;
    }


    return Option<bool>(true);
}