#pragma once
#include "slam_benchmark_suite_kitti.hpp"
#include "srrg_config/configurable_manager.h"

namespace srrg2_core {

  // ds 3D benchmark wrapper for: https://www.mrpt.org/MalagaUrbanDataset
  // ds this dataset contains stereo vision, IMU and laser data
  class SLAMBenchmarkSuiteMalaga : public SLAMBenchmarkSuiteKITTI {
  public:
    EIGEN_MAKE_ALIGNED_OPERATOR_NEW
    using EstimateType = Isometry3f;

    virtual ~SLAMBenchmarkSuiteMalaga() {
    }

    void loadGroundTruth(const std::string& filepath_,
                         const std::string& filepath_additional_ = std::string()) override {
      _ground_truth_path = filepath_;

      // ds open ground truth file in KITTI format: # R t (3x4)
      std::ifstream ground_truth_file(filepath_, std::ios::in);
      if (!ground_truth_file.good() || !ground_truth_file.is_open()) {
        throw std::runtime_error(
          "SLAMBenchmarkSuiteMalaga::loadGroundTruth|unable to read ground truth file: " +
          filepath_);
      }

      // ds KITTI stores timestamps in a separate file
      std::ifstream timestamps_file(filepath_additional_, std::ios::in);
      if (!timestamps_file.good() || !timestamps_file.is_open()) {
        throw std::runtime_error(
          "SLAMBenchmarkSuiteMalaga::loadGroundTruth|unable to read timestamps file: " +
          filepath_additional_);
      }
      _relative_ground_truth_poses.clear();

      // ds relative estimate computation
      double previous_timestamp_seconds = 0;
      Isometry3f previous_pose(Isometry3f::Identity());

      // ds read file by tokens
      double timestamp_seconds = 0;
      double r00, r01, r02, tx, r10, r11, r12, ty, r20, r21, r22, tz;
      while (ground_truth_file >> r00 >> r01 >> r02 >> tx >> r10 >> r11 >> r12 >> ty >> r20 >>
             r21 >> r22 >> tz) {
        // ds assemble isometry
        Isometry3f pose(Isometry3f::Identity());
        pose.translation() = Vector3f(tx, ty, tz);
        pose.linear() << r00, r01, r02, r10, r11, r12, r20, r21, r22;
        _absolute_ground_truth_poses.push_back(pose);

        // ds retrieve timestamp from timestamps file
        std::string buffer;
        if (!std::getline(timestamps_file, buffer)) {
          throw std::runtime_error(
            "SLAMBenchmarkSuiteMalaga::loadGroundTruth|unable to parse timestamp");
        }
        timestamp_seconds = std::stod(buffer);
        _timestamps_seconds_ground_truth.push_back(timestamp_seconds);

        // ds compute relative estimate to previous pose (except for the first measurement)
        if (timestamp_seconds - previous_timestamp_seconds > 0) {
          // ds insertion must succeed otherwise there are duplicates!
          _relative_ground_truth_poses.push_back(RelativeEstimateStamped(
            pose.inverse() * previous_pose, previous_timestamp_seconds, timestamp_seconds));
        }
        previous_timestamp_seconds = timestamp_seconds;
        previous_pose              = pose;
      }

      // ds GPS poses have extremely low frequency (1-2 Hz)
      _maximum_timestamp_delta_seconds = 0.5;
      std::cerr << "SLAMBenchmarkSuiteMalaga::loadGroundTruth|loaded relative poses: "
                << _relative_ground_truth_poses.size() << std::endl;
      ground_truth_file.close();
    }

    void writeTrajectoryToFile(const std::string& filename_ = "trajectory.txt") const override {
      assert(_absolute_ground_truth_poses.size() == _timestamps_seconds_ground_truth.size());
      size_t index_timestamp               = 0;
      constexpr double timestamp_tolerance = 0.1;
      Isometry3f initial_offset(Isometry3f::Identity());
      StdVectorEigenIsometry3f estimated_poses_subsampled;
      estimated_poses_subsampled.reserve(_absolute_ground_truth_poses.size());
      for (const auto& entry : _estimated_poses) {
        if (index_timestamp < _timestamps_seconds_ground_truth.size() &&
            std::fabs(_timestamps_seconds_ground_truth[index_timestamp] - entry.timestamp_seconds) <
              timestamp_tolerance) {
          estimated_poses_subsampled.emplace_back(entry.estimate);
          ++index_timestamp;
        }
      }
      if (estimated_poses_subsampled.size() != _absolute_ground_truth_poses.size()) {
        throw std::runtime_error(
          "SLAMBenchmarkSuiteMalaga::writeTrajectoryToFile|ERROR: inconsistent number of poses");
      }

      // ds since the malaga gt trajectory is quite rough, we need to align our guess first
      Isometry3f transform_estimate_to_ground_truth(Isometry3f::Identity());
      constexpr size_t number_of_iterations = 100;
      constexpr float maximum_error_kernel  = 1; // ds (m^2)
      Matrix6f H;
      Vector6f b;

      // ds perform least squares optimization
      size_t number_of_inliers  = 0;
      float total_error_squared = 0;
      for (size_t iteration = 0; iteration < number_of_iterations; ++iteration) {
        // ds initialize setup
        H.setZero();
        b.setZero();
        number_of_inliers   = 0;
        total_error_squared = 0;

        // ds for all SLAM trajectory poses
        for (size_t index_pose = 0; index_pose < estimated_poses_subsampled.size(); ++index_pose) {
          // ds compute current error
          const Vector3f& measured_point_in_reference =
            _absolute_ground_truth_poses[index_pose].translation();
          const Vector3f sampled_point_in_reference =
            transform_estimate_to_ground_truth *
            estimated_poses_subsampled[index_pose].translation();
          const Vector3f error = sampled_point_in_reference - measured_point_in_reference;

          // ds update chi
          const float error_squared = error.transpose() * error;

          // ds check if outlier
          float weight = 1.0;
          if (error_squared > maximum_error_kernel) {
            weight = maximum_error_kernel / error_squared;
          } else {
            ++number_of_inliers;
          }
          total_error_squared += error_squared;

          // ds get the jacobian of the transform part = [I -2*skew(T*modelPoint)]
          Matrix3_6f jacobian;
          jacobian.block<3, 3>(0, 0).setIdentity();
          jacobian.block<3, 3>(0, 3) = -2 * geometry3d::skew(sampled_point_in_reference);

          // ds precompute transposed
          const Matrix6_3f jacobian_transposed(jacobian.transpose());

          // ds accumulate
          H += weight * jacobian_transposed * jacobian;
          b += weight * jacobian_transposed * error;
        }

        // ds solve the system and update the estimate
        transform_estimate_to_ground_truth =
          geometry3d::v2t(static_cast<const Vector6f&>(H.ldlt().solve(-b))) *
          transform_estimate_to_ground_truth;

        // ds enforce rotation matrix
        const Matrix3f rotation   = transform_estimate_to_ground_truth.linear();
        Matrix3f rotation_squared = rotation.transpose() * rotation;
        rotation_squared.diagonal().array() -= 1;
        transform_estimate_to_ground_truth.linear() -= 0.5 * rotation * rotation_squared;
      }
      std::cerr
        << "SLAMBenchmarkSuiteMalaga::writeTrajectoryToFile|applying estimate to GT alignment: "
        << std::endl;
      std::cerr << transform_estimate_to_ground_truth.matrix() << std::endl;
      std::cerr << "final chi2: " << total_error_squared << " # inliers: " << number_of_inliers
                << " / " << estimated_poses_subsampled.size() << std::endl;

      // ds transform estimated poses brutally and write them to disk
      std::ofstream outfile(filename_, std::ofstream::out);
      if (!outfile.good() || !outfile.is_open()) {
        throw std::runtime_error(
          "SLAMBenchmarkSuiteMalaga::writeTrajectoryToFile|unable to open file: " + filename_);
      }
      outfile << std::fixed;
      outfile << std::setprecision(9);
      for (Isometry3f& estimated_pose : estimated_poses_subsampled) {
        const Isometry3f& estimated_pose_aligned(transform_estimate_to_ground_truth *
                                                 estimated_pose);
        for (size_t r = 0; r < 3; ++r) {
          for (size_t c = 0; c < 4; ++c) {
            outfile << estimated_pose_aligned.matrix()(r, c) << " ";
          }
        }
        outfile << std::endl;
      }
      outfile.close();
      std::cerr << "SLAMBenchmarkSuiteMalaga::writeTrajectoryToFile|saved messages (subsampled): "
                << estimated_poses_subsampled.size() << "/" << _estimated_poses.size() << std::endl;
    }

  protected:
    // ds ground truth log for hacky subsampled/aligned (!) output
    StdDequeEigenIsometry3f _absolute_ground_truth_poses;
    std::vector<double> _timestamps_seconds_ground_truth;
  }; // namespace srrg2_core

} // namespace srrg2_core
