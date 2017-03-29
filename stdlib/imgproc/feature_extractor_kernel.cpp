#include "scanner/api/kernel.h"
#include "scanner/api/op.h"
#include "scanner/util/cuda.h"
#include "scanner/util/memory.h"
#include "scanner/util/opencv.h"
#include "scanner/util/serialize.h"
#include "stdlib/stdlib.pb.h"

#include <opencv2/xfeatures2d.hpp>

namespace scanner {

class FeatureExtractorKernel : public VideoKernel {
public:
  FeatureExtractorKernel(const Kernel::Config& config)
    : VideoKernel(config),
      device_(config.devices[0]) {
    set_device();

    if (!args_.ParseFromArray(config.args.data(), config.args.size())) {
      LOG(FATAL) << "Failed to parse args";
    }

    if (args_.feature_type() == proto::ExtractorType::SIFT) {
      if (device_.type == DeviceType::GPU) {
        LOG(FATAL) << "GPU SIFT not supported yet";
      } else {
        cpu_extractor_ = cv::xfeatures2d::SIFT::create();
      }
    } else if (args_.feature_type() == proto::ExtractorType::SURF) {
      if (device_.type == DeviceType::GPU) {
        gpu_extractor_ = new cvc::SURF_CUDA(100);
      } else {
        cpu_extractor_ = cv::xfeatures2d::SURF::create();
      }
    } else {
      LOG(FATAL) << "Invalid feature type";
    }
  }

  void execute(const BatchedColumns& input_columns,
               BatchedColumns& output_columns) override {
    set_device();

    auto& frame_col = input_columns[0];
    auto& frame_info_col = input_columns[1];
    check_frame_info(device_, frame_info_col);

    i32 input_count = input_columns[0].rows.size();

    std::vector<std::vector<cv::KeyPoint>> keypoints;
    std::vector<std::tuple<u8*, size_t>> features;
    keypoints.resize(input_count);

    std::vector<cvc::GpuMat> feat_gpus;

    if (device_.type == DeviceType::GPU) {
      if(args_.feature_type() == proto::ExtractorType::SURF) {
        cvc::SURF_CUDA* surf = (cvc::SURF_CUDA*) gpu_extractor_;
        feat_gpus.resize(input_count);
        for (i32 i = 0; i < input_count; ++i) {
          cvc::GpuMat kp_gpu;
          cvc::GpuMat img(frame_info_.height(), frame_info_.width(), CV_8UC3,
                          frame_col.rows[i].buffer);
          cvc::cvtColor(img, img, CV_RGB2GRAY);
          (*surf)(img, cvc::GpuMat(), kp_gpu, feat_gpus[i]);
          surf->downloadKeypoints(kp_gpu, keypoints[i]);
          features.push_back(
            std::make_tuple(
              feat_gpus[i].data,
              feat_gpus[i].rows * feat_gpus[i].cols * feat_gpus[i].elemSize()));
        }
      }
    } else {
      std::vector<cv::Mat> imgs;
      for (i32 i = 0; i < input_count; ++i) {
        cv::Mat img(frame_info_.height(), frame_info_.width(), CV_8UC3,
                    (u8*)frame_col.rows[i].buffer);
        imgs.push_back(img);
      }

      std::vector<cv::Mat> cv_features;
      cpu_extractor_->compute(imgs, keypoints, features);

      for (i32 i = 0; i < input_count; ++i) {
        cv::Mat m = cv_features[i];
        features.push_back(std::make_tuple(m.data, m.total() * m.elemSize()));
      }
    }

#define OR_4(N) std::max((N), (size_t)4)

    for (i32 i = 0; i < input_count; ++i) {
      u8* cv_buf = std::get<0>(features[i]);
      size_t size = std::get<1>(features[i]);
      u8* output_buf = new_buffer(device_, OR_4(size));
      memcpy_buffer(output_buf, device_,
                    cv_buf, device_,
                    size);
      INSERT_ROW(output_columns[0], output_buf, OR_4(size));

      std::vector<proto::Keypoint> kps_proto;
      for (auto& kp : keypoints[i]) {
        proto::Keypoint kp_proto;
        kp_proto.set_x(kp.pt.x);
        kp_proto.set_y(kp.pt.y);
        kps_proto.push_back(kp_proto);
      }

      serialize_proto_vector(kps_proto, output_buf, size);
      if (device_.type == DeviceType::GPU) {
        u8* gpu_buf = new_buffer(device_, OR_4(size));
        memcpy_buffer(gpu_buf, device_, output_buf, CPU_DEVICE, size);
        delete_buffer(CPU_DEVICE, output_buf);
        output_buf = gpu_buf;
      }
      INSERT_ROW(output_columns[1], output_buf, OR_4(size));
    }
  }

  void set_device() {
    CUDA_PROTECT({ CU_CHECK(cudaSetDevice(device_.id)); });
    cvc::setDevice(device_.id);
  }

private:
  DeviceHandle device_;
  proto::FeatureExtractorArgs args_;
  void* gpu_extractor_;
  cv::Ptr<cv::Feature2D> cpu_extractor_;
};

REGISTER_OP(FeatureExtractor).inputs({"frame", "frame_info"}).outputs({"features", "keypoints"});

REGISTER_KERNEL(FeatureExtractor, FeatureExtractorKernel)
    .device(DeviceType::GPU)
    .num_devices(1);
}