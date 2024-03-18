/* Copyright (C) 2021 Steffen Urban
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "OpenCameraCalibrator/core/camera_calibrator.h"

#include <opencv2/aruco/charuco.hpp>
#include <opencv2/aruco/dictionary.hpp>
#include <opencv2/opencv.hpp>
#include <theia/io/reconstruction_writer.h>
#include <theia/io/write_ply_file.h>
#include <theia/sfm/bundle_adjustment/bundle_adjuster.h>
#include <theia/sfm/bundle_adjustment/bundle_adjustment.h>
#include <theia/sfm/estimators/estimate_radial_dist_uncalibrated_absolute_pose.h>
#include <theia/sfm/estimators/estimate_uncalibrated_absolute_pose.h>
#include <theia/sfm/estimators/feature_correspondence_2d_3d.h>
#include <theia/solvers/ransac.h>
// camera types
#include <theia/sfm/camera/division_undistortion_camera_model.h>
#include <theia/sfm/camera/double_sphere_camera_model.h>
#include <theia/sfm/camera/extended_unified_camera_model.h>
#include <theia/sfm/camera/fisheye_camera_model.h>
#include <theia/sfm/camera/pinhole_camera_model.h>
#include <theia/sfm/camera/pinhole_radial_tangential_camera_model.h>

#include "OpenCameraCalibrator/io/read_scene.h"
#include "OpenCameraCalibrator/io/write_camera_calibration.h"
#include "OpenCameraCalibrator/utils/intrinsic_initializer.h"
#include "OpenCameraCalibrator/utils/json.h"
#include "OpenCameraCalibrator/utils/types.h"
#include "OpenCameraCalibrator/utils/utils.h"

#include <thread>

namespace OpenICC {
namespace core {

CameraCalibrator::CameraCalibrator(const std::string& camera_model,
                                   const bool optimize_board_pts)
    : camera_model_(camera_model), optimize_board_pts_(optimize_board_pts) {
  ransac_params_.failure_probability = 0.001;
  ransac_params_.use_mle = true;
  ransac_params_.max_iterations = 1000;
  ransac_params_.min_iterations = 5;
  ransac_params_.error_thresh = 3.0;
}

void CameraCalibrator::RemoveViewsReprojError(const double max_reproj_error) {
  // reproj error per view, remove some views which have a high error
  std::map<theia::ViewId, double> ids_to_remove;
  for (int i = 0; i < recon_calib_dataset_.NumViews(); ++i) {
    const theia::ViewId v_id = recon_calib_dataset_.ViewIds()[i];
    const double view_reproj_error =
        utils::GetReprojErrorOfView(recon_calib_dataset_, v_id);
    if (view_reproj_error > max_reproj_error) {
      ids_to_remove[v_id] = view_reproj_error;
    }
  }
  for (auto v_id : ids_to_remove) {
    recon_calib_dataset_.RemoveView(v_id.first);
    LOG(INFO) << "Removed view: " << v_id.first
              << " with RMSE reproj error: " << v_id.second << "\n";
  }
}

bool CameraCalibrator::AddObservation(const theia::ViewId& view_id,
                                      const theia::TrackId& object_point_id,
                                      const Eigen::Vector2d& corner) {
  return recon_calib_dataset_.AddObservation(view_id, object_point_id, corner);
}

theia::ViewId CameraCalibrator::AddView(
    const Eigen::Matrix3d& initial_rotation,
    const Eigen::Vector3d& initial_position,
    const double& initial_focal_length,
    const double& initial_distortion,
    const int& image_width,
    const int& image_height,
    const double& timestamp_s,
    const theia::CameraIntrinsicsGroupId group_id) {
  // fill charucoCorners to theia reconstruction
  std::string view_name = std::to_string((uint64_t)(timestamp_s * S_TO_US));
  theia::ViewId view_id =
      recon_calib_dataset_.AddView(view_name, group_id, timestamp_s);
  theia::View* theia_view = recon_calib_dataset_.MutableView(view_id);
  theia_view->SetEstimated(true);

  // initialize intrinsics
  theia::Camera* cam = theia_view->MutableCamera();
  cam->SetImageSize(image_width, image_height);
  cam->SetPrincipalPoint(image_width / 2.0, image_height / 2.0);
  cam->SetPosition(initial_position);
  cam->SetOrientationFromRotationMatrix(initial_rotation);
  cam->SetFocalLength(initial_focal_length);
  cam->SetCameraIntrinsicsModelType(
      theia::StringToCameraIntrinsicsModelType(camera_model_));

  if (camera_model_ == "PINHOLE") {
  } else if (camera_model_ == "DIVISION_UNDISTORTION") {
    cam->CameraIntrinsics()->SetParameter(
        theia::DivisionUndistortionCameraModel::RADIAL_DISTORTION_1,
        initial_distortion);
  } else if (camera_model_ == "DOUBLE_SPHERE") {
    cam->CameraIntrinsics()->SetParameter(theia::DoubleSphereCameraModel::XI,
                                          -0.25);
    cam->CameraIntrinsics()->SetParameter(theia::DoubleSphereCameraModel::ALPHA,
                                          0.5);
  } else if (camera_model_ == "EXTENDED_UNIFIED") {
    cam->CameraIntrinsics()->SetParameter(
        theia::ExtendedUnifiedCameraModel::ALPHA, 0.5);
    cam->CameraIntrinsics()->SetParameter(
        theia::ExtendedUnifiedCameraModel::BETA, 1.0);
  } else if (camera_model_ == "FISHEYE") {
  } else if (camera_model_ == "PINHOLE_RADIAL_TANGENTIAL") {
  }

  return view_id;
}

bool CameraCalibrator::RunCalibration() {
  if (recon_calib_dataset_.NumViews() < min_num_view_) {
    LOG(ERROR) << "Not enough views for proper calibration!" << std::endl;
    return false;
  }

  std::cout << "Using " << recon_calib_dataset_.NumViews()
            << " views for camera calibration.\n";
  // bundle adjust everything
  theia::BundleAdjustmentOptions ba_options;
  ba_options.verbose = true;
  ba_options.loss_function_type = theia::LossFunctionType::HUBER;
  ba_options.robust_loss_width = 1.345;
  ba_options.num_threads = std::thread::hardware_concurrency();

  /////////////////////////////////////////////////
  /// 1. Optimize focal length and radial distortion, keep principal point fixed
  /////////////////////////////////////////////////
  ba_options.constant_camera_orientation = false;
  ba_options.constant_camera_position = false;
  ba_options.intrinsics_to_optimize =
      theia::OptimizeIntrinsicsType::FOCAL_LENGTH;
  if (camera_model_ != "PINHOLE") {
    ba_options.intrinsics_to_optimize |=
        theia::OptimizeIntrinsicsType::RADIAL_DISTORTION;
  }
  LOG(INFO) << "Bundle adjusting focal length and radial distortion.\n";

  theia::BundleAdjustmentSummary summary = BundleAdjustViews(
      ba_options, recon_calib_dataset_.ViewIds(), &recon_calib_dataset_);

  RemoveViewsReprojError(5.0);

  /////////////////////////////////////////////////
  /// 2. Optimize principal point keeping everything else fixed
  /////////////////////////////////////////////////
  LOG(INFO) << "Optimizing principal point.";
  ba_options.constant_camera_orientation = true;
  ba_options.constant_camera_position = true;
  ba_options.intrinsics_to_optimize =
      theia::OptimizeIntrinsicsType::PRINCIPAL_POINTS;

  summary = theia::BundleAdjustViews(
      ba_options, recon_calib_dataset_.ViewIds(), &recon_calib_dataset_);

  if (recon_calib_dataset_.NumViews() < min_num_view_) {
    std::cout << "Not enough views left for proper calibration!" << std::endl;
    return false;
  }

  /////////////////////////////////////////////////
  /// 3. Full optimization
  /////////////////////////////////////////////////
  ba_options.constant_camera_orientation = false;
  ba_options.constant_camera_position = false;
  ba_options.intrinsics_to_optimize =
      theia::OptimizeIntrinsicsType::PRINCIPAL_POINTS |
      theia::OptimizeIntrinsicsType::FOCAL_LENGTH |
      theia::OptimizeIntrinsicsType::ASPECT_RATIO;

  if (camera_model_ == "PINHOLE") {
    ba_options.intrinsics_to_optimize |=
        theia::OptimizeIntrinsicsType::RADIAL_DISTORTION;
  } else if (camera_model_ == "PINHOLE_RADIAL_TANGENTIAL") {
    ba_options.intrinsics_to_optimize |=
        theia::OptimizeIntrinsicsType::TANGENTIAL_DISTORTION;
  }
  summary = theia::BundleAdjustViews(
      ba_options, recon_calib_dataset_.ViewIds(), &recon_calib_dataset_);

  RemoveViewsReprojError(2.0);

  if (recon_calib_dataset_.NumViews() < min_num_view_) {
    std::cout << "Not enough views left for proper calibration!" << std::endl;
    return false;
  }

  if (optimize_board_pts_) {
    LOG(INFO) << "Optimizing board points.";
    ba_options.use_homogeneous_point_parametrization = true;
    ba_options.verbose = true;
    theia::BundleAdjustTracks(
        ba_options, recon_calib_dataset_.TrackIds(), &recon_calib_dataset_);
    summary = theia::BundleAdjustViews(
        ba_options, recon_calib_dataset_.ViewIds(), &recon_calib_dataset_);
  }

  return true;
}

bool CameraCalibrator::CalibrateCameraFromJson(const nlohmann::json& scene_json,
                                               const std::string& output_path) {
  io::scene_points_to_calib_dataset(scene_json, recon_calib_dataset_);

  const int image_width = scene_json["image_width"];
  const int image_height = scene_json["image_height"];
  // initial principal point
  const double px = static_cast<double>(image_width) / 2.0;
  const double py = static_cast<double>(image_height) / 2.0;

  vec3_vector saved_poses;
  // iterate views and estimate poses
  const auto views = scene_json["views"];
  const size_t total_nr_views = views.size();
  int views_initialized = 0;
  for (const auto& view : views.items()) {
    const double timestamp_us = std::stod(view.key());  // to seconds
    const double timestamp_s = timestamp_us * 1e-6;     // to seconds
    const auto image_points = view.value()["image_points"];
    std::vector<int> board_pt3_ids;
    aligned_vector<Eigen::Vector2d> corners;
    for (const auto& img_pts : image_points.items()) {
      board_pt3_ids.push_back(std::stoi(img_pts.key()));
      corners.push_back(
          Eigen::Vector2d(img_pts.value()[0], img_pts.value()[1]));
    }

    LOG(INFO) << "Initializing view at timestamp: " << timestamp_s << "\n";
    // initialize cam pose
    std::vector<theia::FeatureCorrespondence2D3D> correspondences(
        board_pt3_ids.size());
    for (size_t i = 0; i < board_pt3_ids.size(); ++i) {
      theia::FeatureCorrespondence2D3D correspondence;
      correspondence.feature[0] = corners[i][0] - px;
      correspondence.feature[1] = corners[i][1] - py;
      const Eigen::Vector4d track =
          recon_calib_dataset_.Track(board_pt3_ids[i])->Point();
      correspondence.world_point = track.hnormalized();
      correspondences[i] = correspondence;
    }

    theia::RansacSummary ransac_summary;
    Eigen::Matrix3d rotation;
    Eigen::Vector3d position;
    bool success_init = false;
    double focal_length = 0.0, radial_distortion = 0.0;
    LOG(INFO) << "Initializing " << camera_model_ << " camera model.\n";

    // set error thresh 0.3% from image size
    ransac_params_.error_thresh = 0.003 * image_height;
    if (camera_model_ == "PINHOLE" ||
        camera_model_ == "PINHOLE_RADIAL_TANGENTIAL") {
      success_init = utils::initialize_pinhole_camera(correspondences,
                                                      ransac_params_,
                                                      ransac_summary,
                                                      rotation,
                                                      position,
                                                      focal_length,
                                                      verbose_);
    } else if (camera_model_ == "DIVISION_UNDISTORTION") {
      success_init = utils::initialize_radial_undistortion_camera(
          correspondences,
          ransac_params_,
          ransac_summary,
          cv::Size(image_width, image_height),
          rotation,
          position,
          focal_length,
          radial_distortion,
          verbose_);
    } else {
      success_init = utils::initialize_radial_undistortion_camera(
          correspondences,
          ransac_params_,
          ransac_summary,
          cv::Size(image_width, image_height),
          rotation,
          position,
          focal_length,
          radial_distortion,
          verbose_);
      //        success_init = utils::initialize_doublesphere_model(
      //                correspondences, board_pt3_ids, cv::Size(9, 7),
      //                ransac_params_, image_width, image_height,
      //                ransac_summary, rotation, position, focal_length,
      //                verbose_);
    }
    if (views_initialized % 100 == 0) {
      std::cout << "View: " << views_initialized << "/" << total_nr_views
                << " initialized for calibration.\n";
    }
    ++views_initialized;

    // check if a very close by pose is already present
    bool take_image = true;
    for (size_t i = 0; i < saved_poses.size(); ++i) {
      if ((position - saved_poses[i]).norm() < grid_size_) {
        take_image = false;
        break;
      }
    }

    if (!take_image || !success_init) {
      continue;
    }

    saved_poses.push_back(position);

    theia::ViewId view_id = AddView(rotation,
                                    position,
                                    focal_length,
                                    radial_distortion,
                                    image_width,
                                    image_height,
                                    timestamp_s);

    for (size_t i = 0; i < board_pt3_ids.size(); ++i) {
      AddObservation(view_id, board_pt3_ids[i], corners[i]);
    }
  }

  theia::WritePlyFile(output_path + "_ransac_poses.ply",
                      recon_calib_dataset_,
                      Eigen::Vector3i(255, 0, 0),
                      1);

  if (!RunCalibration()) {
    LOG(ERROR) << "Calibration failed.\n";
    return false;
  }

  // final reprojection error
  double reproj_error = 0;
  for (int i = 0; i < recon_calib_dataset_.NumViews(); ++i) {
    const double view_reproj_error = utils::GetReprojErrorOfView(
        recon_calib_dataset_, recon_calib_dataset_.ViewIds()[i]);
    reproj_error += view_reproj_error;
    if (verbose_) {
      LOG(INFO) << "View: " << recon_calib_dataset_.ViewIds()[i]
                << " RMSE reprojection error: " << view_reproj_error << "\n";
    }
  }

  const double total_repro_error =
      reproj_error / recon_calib_dataset_.NumViews();
  std::cout << "Final camera calibration reprojection error: "
            << total_repro_error << " from " << recon_calib_dataset_.NumViews()
            << " view." << std::endl;
  const theia::Camera cam =
      recon_calib_dataset_.View(recon_calib_dataset_.ViewIds()[0])->Camera();

  if (output_path != "") {
    theia::WriteReconstruction(recon_calib_dataset_,
                               output_path + ".calibdata");
    CHECK(io::write_camera_calibration(output_path + ".json",
                                       cam,
                                       scene_json["camera_fps"],
                                       recon_calib_dataset_.NumViews(),
                                       total_repro_error))
        << "Could not write calibration file.\n";
    theia::WritePlyFile(output_path + "_final_poses.ply",
                        recon_calib_dataset_,
                        Eigen::Vector3i(255, 0, 0),
                        1);
  }

  return true;
}

void CameraCalibrator::PrintResult() {
  const theia::Camera cam =
      recon_calib_dataset_.View(recon_calib_dataset_.ViewIds()[0])->Camera();
  std::cout << "Focal Length:" << cam.FocalLength()
            << "px Principal Point: " << cam.PrincipalPointX() << "/"
            << cam.PrincipalPointY() << "px.\n";
  if (camera_model_ == "DIVISION_UNDISTORTION") {
    std::cout
        << "DIVISION_UNDISTORTION model: "
        << "Distortion: "
        << cam.intrinsics()[theia::DivisionUndistortionCameraModel::
                                InternalParametersIndex::RADIAL_DISTORTION_1]
        << "\n";
  } else if (camera_model_ == "DOUBLE_SPHERE") {
    std::cout
        << "DOUBLE_SPHERE model: "
        << "XI: "
        << cam.intrinsics()
               [theia::DoubleSphereCameraModel::InternalParametersIndex::XI]
        << " ALPHA: "
        << cam.intrinsics()
               [theia::DoubleSphereCameraModel::InternalParametersIndex::ALPHA]
        << "\n";
  } else if (camera_model_ == "EXTENDED_UNIFIED") {
    std::cout << "EXTENDED_UNIFIED model: "
              << cam.intrinsics()[theia::ExtendedUnifiedCameraModel::
                                      InternalParametersIndex::ALPHA]
              << " BETA: "
              << cam.intrinsics()[theia::ExtendedUnifiedCameraModel::
                                      InternalParametersIndex::BETA]
              << "\n";
  } else if (camera_model_ == "FISHEYE") {
    std::cout
        << "FISHEYE model: "
        << "Radial distortion 1: "
        << cam.intrinsics()[theia::FisheyeCameraModel::InternalParametersIndex::
                                RADIAL_DISTORTION_1]
        << " Radial distortion 2: "
        << cam.intrinsics()[theia::FisheyeCameraModel::InternalParametersIndex::
                                RADIAL_DISTORTION_2]
        << " Radial distortion 3: "
        << cam.intrinsics()[theia::FisheyeCameraModel::InternalParametersIndex::
                                RADIAL_DISTORTION_3]
        << " Radial distortion 4: "
        << cam.intrinsics()[theia::FisheyeCameraModel::InternalParametersIndex::
                                RADIAL_DISTORTION_4]
        << "\n";
  } else if (camera_model_ == "PINHOLE_DISTORTION") {
    std::cout
        << "Pinhole with radial-tangential distortion: "
        << "Radial distortion 1: "
        << cam.intrinsics()[theia::PinholeRadialTangentialCameraModel::
                                InternalParametersIndex::RADIAL_DISTORTION_1]
        << " Radial distortion 2: "
        << cam.intrinsics()[theia::PinholeRadialTangentialCameraModel::
                                InternalParametersIndex::RADIAL_DISTORTION_2]
        << " Radial distortion 3: "
        << cam.intrinsics()[theia::PinholeRadialTangentialCameraModel::
                                InternalParametersIndex::RADIAL_DISTORTION_3]
        << " Tangential distortion 1: "
        << cam.intrinsics()
               [theia::PinholeRadialTangentialCameraModel::
                    InternalParametersIndex::TANGENTIAL_DISTORTION_1]
        << " Tangential distortion 2: "
        << cam.intrinsics()
               [theia::PinholeRadialTangentialCameraModel::
                    InternalParametersIndex::TANGENTIAL_DISTORTION_2]
        << "\n";
  }
}
}  // namespace core
}  // namespace OpenICC
