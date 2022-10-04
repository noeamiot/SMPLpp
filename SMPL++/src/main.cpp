/* ========================================================================= *
 *                                                                           *
 *                                 SMPL++                                    *
 *                    Copyright (c) 2018, Chongyi Zheng.                     *
 *                          All Rights reserved.                             *
 *                                                                           *
 * ------------------------------------------------------------------------- *
 *                                                                           *
 * This software implements a 3D human skinning model - SMPL: A Skinned      *
 * Multi-Person Linear Model with C++.                                       *
 *                                                                           *
 * For more detail, see the paper published by Max Planck Institute for      *
 * Intelligent Systems on SIGGRAPH ASIA 2015.                                *
 *                                                                           *
 * We provide this software for research purposes only.                      *
 * The original SMPL model is available at http://smpl.is.tue.mpg.           *
 *                                                                           *
 * ========================================================================= */

//=============================================================================
//
//  APPLICATION ENTRANCE
//
//=============================================================================

//===== MACROS ================================================================

#define SINGLE_SMPL smpl::Singleton<smpl::SMPL>

//===== INCLUDES ==============================================================

//----------
#include <chrono>
//----------
#include <torch/torch.h>
//----------
#include <xtensor/xadapt.hpp>
#include <xtensor/xarray.hpp>
//----------
#include "definition/def.h"
#include "smpl/SMPL.h"
#include "toolbox/Singleton.hpp"
#include "toolbox/TorchEx.hpp"
//----------
#include <ros/package.h>
#include <ros/ros.h>
#include <std_msgs/Float64MultiArray.h>
#include <visualization_msgs/MarkerArray.h>
#include <smplpp/PoseParam.h>
//----------

//===== FORWARD DECLARATIONS ==================================================

//===== NAMESPACE =============================================================

//===== MAIN FUNCTION =========================================================

torch::Tensor g_theta = torch::zeros({BATCH_SIZE, JOINT_NUM, 3}); // (N, 24, 3)
torch::Tensor g_beta = torch::zeros({BATCH_SIZE, SHAPE_BASIS_DIM}); // (N, 10)

void poseParamCallback(const smplpp::PoseParam::ConstPtr & msg)
{
  if(msg->angles.size() != JOINT_NUM)
  {
    ROS_WARN_STREAM("Invalid pose param size: " << std::to_string(msg->angles.size())
                                                << " != " << std::to_string(JOINT_NUM));
    return;
  }

  for(int64_t i = 0; i < JOINT_NUM; i++)
  {
    g_theta.index({0, i, 0}) = msg->angles[i].x;
    g_theta.index({0, i, 1}) = msg->angles[i].y;
    g_theta.index({0, i, 2}) = msg->angles[i].z;
  }
}

void shapeParamCallback(const std_msgs::Float64MultiArray::ConstPtr & msg)
{
  if(msg->data.size() != SHAPE_BASIS_DIM)
  {
    ROS_WARN_STREAM("Invalid shape param size: " << std::to_string(msg->data.size())
                                                 << " != " << std::to_string(SHAPE_BASIS_DIM));
    return;
  }

  for(int64_t i = 0; i < SHAPE_BASIS_DIM; i++)
  {
    g_beta.index({0, i}) = msg->data[i];
  }
}

int main(int argc, char * argv[])
{
  // Setup ROS
  ros::init(argc, argv, "smplpp");
  ros::NodeHandle nh;
  ros::NodeHandle pnh("~");
  ros::Publisher markerPub = nh.advertise<visualization_msgs::MarkerArray>("smplpp/marker_arr", 1);
  ros::Subscriber poseParamSub = nh.subscribe("smplpp/pose_param", 1, poseParamCallback);
  ros::Subscriber shapeParamSub = nh.subscribe("smplpp/shape_param", 1, shapeParamCallback);

  // Load SMPL model
  {
    auto startTime = std::chrono::system_clock::now();

    std::string deviceType = "CPU";
    pnh.getParam("device", deviceType);
    std::unique_ptr<torch::Device> device;
    if(deviceType == "CPU")
    {
      device = std::make_unique<torch::Device>(torch::kCPU);
    }
    else if(deviceType == "CUDA")
    {
      device = std::make_unique<torch::Device>(torch::kCUDA);
    }
    else
    {
      throw smpl::smpl_error("main", "Invalid device type: " + deviceType);
    }
    device->set_index(0);
    ROS_INFO_STREAM("Device type: " << deviceType);

    std::string modelPath;
    pnh.getParam("model_path", modelPath);
    if(modelPath.empty())
    {
      modelPath = ros::package::getPath("smplpp") + "/data/smpl_female.json";
    }
    ROS_INFO_STREAM("Load SMPL model from " << modelPath);

    SINGLE_SMPL::get()->setDevice(*device);
    SINGLE_SMPL::get()->setModelPath(modelPath);
    SINGLE_SMPL::get()->init();

    ROS_INFO_STREAM("Duration to load SMPL: " << std::chrono::duration_cast<std::chrono::duration<double>>(
                                                     std::chrono::system_clock::now() - startTime)
                                                     .count()
                                              << " [s]");
  }

  double rateFreq = 30.0;
  pnh.getParam("rate", rateFreq);
  ros::Rate rate(rateFreq);
  while(ros::ok())
  {
    // Update SMPL model
    {
      auto startTime = std::chrono::system_clock::now();

      SINGLE_SMPL::get()->launch(g_beta, g_theta);

      ROS_INFO_STREAM_THROTTLE(10.0,
                               "Duration to update SMPL: " << std::chrono::duration_cast<std::chrono::duration<double>>(
                                                                  std::chrono::system_clock::now() - startTime)
                                                                      .count()
                                                                  * 1e3
                                                           << " [ms]");
    }

    {
      auto startTime = std::chrono::system_clock::now();

      visualization_msgs::MarkerArray markerArrMsg;
      visualization_msgs::Marker markerMsg;
      markerMsg.header.stamp = ros::Time::now();
      markerMsg.header.frame_id = "world";
      markerMsg.ns = "SMPL model";
      markerMsg.id = 0;
      markerMsg.type = visualization_msgs::Marker::TRIANGLE_LIST;
      markerMsg.action = visualization_msgs::Marker::ADD;
      markerMsg.pose.orientation.w = 1.0;
      markerMsg.scale.x = 1.0;
      markerMsg.scale.y = 1.0;
      markerMsg.scale.z = 1.0;
      markerMsg.color.r = 0.1;
      markerMsg.color.g = 0.8;
      markerMsg.color.b = 0.1;
      markerMsg.color.a = 1.0;

      torch::Tensor vertexTensorBatched = SINGLE_SMPL::get()->getVertex();
      torch::Tensor faceIndexTensor = SINGLE_SMPL::get()->getFaceIndex();
      assert(vertexTensorBatched.sizes() == torch::IntArrayRef({BATCH_SIZE, VERTEX_NUM, 3}));
      assert(faceIndexTensor.sizes() == torch::IntArrayRef({FACE_INDEX_NUM, 3}));

      torch::Tensor vertexTensor = smpl::TorchEx::indexing(vertexTensorBatched, torch::IntArrayRef({0}));
      xt::xarray<float> vertexArr =
          xt::adapt(vertexTensor.to(torch::kCPU).data_ptr<float>(),
                    xt::xarray<float>::shape_type({static_cast<const size_t>(VERTEX_NUM), 3}));

      xt::xarray<int32_t> faceIndexArr =
          xt::adapt(faceIndexTensor.to(torch::kCPU).data_ptr<int32_t>(),
                    xt::xarray<int32_t>::shape_type({static_cast<const size_t>(FACE_INDEX_NUM), 3}));

      for(int64_t i = 0; i < FACE_INDEX_NUM; i++)
      {
        for(int64_t j = 0; j < 3; j++)
        {
          int64_t idx = faceIndexArr(i, j) - 1;
          geometry_msgs::Point pointMsg;
          pointMsg.x = vertexArr(idx, 0);
          pointMsg.y = vertexArr(idx, 1);
          pointMsg.z = vertexArr(idx, 2);
          markerMsg.points.push_back(pointMsg);
        }
      }

      markerArrMsg.markers.push_back(markerMsg);
      markerPub.publish(markerArrMsg);

      ROS_INFO_STREAM_THROTTLE(10.0, "Duration to publish message: "
                                         << std::chrono::duration_cast<std::chrono::duration<double>>(
                                                std::chrono::system_clock::now() - startTime)
                                                    .count()
                                                * 1e3
                                         << " [ms]");
    }

    ros::spinOnce();
    rate.sleep();
  }

  SINGLE_SMPL::destroy();

  return 0;
}

//===== CLEAN AFTERWARD =======================================================

#undef SINGLE_SMPL

//=============================================================================
