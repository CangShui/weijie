#include <utility>
#include <fstream>
#include <algorithm>
#include <iterator>

#include "plog/Log.h"

#include "crnn_net.h"
#include "utils.h"

namespace OCR
{

CRNNNet::CRNNNet(const RecConfig &config)
{
    Initialize(config);
}

CRNNNet::CRNNNet(CRNNNet &&other) noexcept
    : config_(std::exchange(other.config_, {}))
    , net_(std::move(other.net_))
    , keys_(std::move(other.keys_))
{

}

CRNNNet & CRNNNet::operator = (CRNNNet &&other) noexcept
{
    if (this != &other)
    {
        config_ = std::exchange(other.config_, {});
        net_ = std::move(other.net_);
        keys_ = std::move(other.keys_);
    }
    return *this;
}

bool CRNNNet::Initialize(const RecConfig &config)
{
    config_ = config;

    // create net
    net_ = std::make_unique<ncnn::Net>();
    net_->opt.num_threads = config_.infer_threads;
    net_->opt.use_fp16_packed = config_.is_fp16;
    net_->opt.use_fp16_storage = config_.is_fp16;
    net_->opt.use_fp16_arithmetic = config_.is_fp16;
    net_->opt.use_vulkan_compute = config_.use_vulkan;

    // PaddleOCR lists characters in class order: class 0 is the CTC blank and
    // the last class is a space. The small dictionary spells both out, the tiny
    // and medium ones do not, so normalize every dictionary to that layout.
    std::ifstream ifs{config_.keys_path};
    if (!ifs.is_open())
    {
        PLOGE << "Failed to load keys " << config_.keys_path;
        return false;
    }
    std::vector<std::string> chars;
    std::string line;
    while (std::getline(ifs, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        chars.emplace_back(line);
    }
    if (!chars.empty() && chars.front().empty()) chars.erase(chars.begin());
    if (chars.empty() || chars.back() != " ") chars.emplace_back(" ");
    keys_.clear();
    keys_.reserve(chars.size() + 1);
    keys_.emplace_back();
    keys_.insert(keys_.end(), chars.begin(), chars.end());
    PLOGD << "Total keys: " << keys_.size();

    if (net_->load_param((config_.model_path + ".param").c_str()) ||
        net_->load_model((config_.model_path + ".bin").c_str()))
    {
        PLOGE << "Failed to load model " << config_.model_path;
        net_.reset();
        return false;
    }

    if (!net_->input_names().empty() && net_->input_names()[0])
        input_name_ = net_->input_names()[0];
    if (!net_->output_names().empty() && net_->output_names()[0])
        output_name_ = net_->output_names()[0];
    PLOGD << "Rec io: " << input_name_ << " -> " << output_name_;

    return true;
}

std::vector<TextLine> CRNNNet::Rec(const std::vector<cv::Mat> &text_images) const
{
    std::vector<TextLine> text_lines(text_images.size());

    int num_lines = static_cast<int>(text_images.size());
    #pragma omp parallel for num_threads(config_.reco_threads) schedule(dynamic)
    for (int i = 0; i < num_lines; ++i)
    {
        text_lines[i] = Rec(text_images[i]);
    }

    return text_lines;
}

TextLine CRNNNet::Rec(const cv::Mat &text_image) const
{
    // resize
    const int target_h = config_.target_h;
    float ratio = static_cast<float>(target_h) / text_image.rows;
    int rsz_w = std::max(static_cast<int>(text_image.cols * ratio), 1);

    ncnn::Mat blob = ncnn::Mat::from_pixels_resize(text_image.data, ncnn::Mat::PIXEL_BGR,
        text_image.cols, text_image.rows, rsz_w, target_h);
    blob.substract_mean_normalize(mean_values_, norm_values_);

    // inference
    ncnn::Extractor ex = net_->create_extractor();
    ex.input(input_name_.c_str(), blob);
    ncnn::Mat out;
    ex.extract(output_name_.c_str(), out);
    if (out.empty())
    {
        PLOGE << "Recognition returned no output, check " << config_.model_path;
        return TextLine{};
    }

    // decode output and get TextLine
    float *arr = reinterpret_cast<float *>(out.data);
    std::vector<float> scores(arr, arr + out.h * out.w);

    return Score2TextLine(scores, out.h, out.w);
}

TextLine CRNNNet::Score2TextLine(const std::vector<float> &scores, const int rows, const int cols) const
{
    if (cols != static_cast<int>(keys_.size()))
    {
        PLOGE << "Unmatched scores: " << cols << " != " << keys_.size()
              << " (" << config_.model_path << " / " << config_.keys_path << ")";
        return TextLine{};
    }

    std::string text;
    std::vector<float> text_scores;
    int prev_i = -1;
    const int blank_i = 0;

    for (int i = 0; i < rows; ++i)
    {
        auto max_it = std::max_element(scores.begin() + i * cols, scores.begin() + (i + 1) * cols);
        int max_i = static_cast<int>(std::distance(scores.begin(), max_it));
        max_i %= cols;
        float max_v = *max_it;

        if (max_i != blank_i && max_i != prev_i)
        {
            text.append(keys_[max_i]);
            text_scores.emplace_back(max_v);
        }
        prev_i = max_i;
    }

    Trim(text);

    return {std::move(text), std::move(text_scores)};
}

}   // namespace OCR
