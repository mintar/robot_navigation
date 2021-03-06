/*
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2017, Locus Robotics
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

#include <nav_core2/exceptions.h>
#include <dwb_local_planner/dwb_local_planner.h>
#include <dwb_local_planner/backwards_compatibility.h>
#include <dwb_local_planner/illegal_trajectory_tracker.h>
#include <nav_2d_utils/conversions.h>
#include <nav_2d_utils/tf_help.h>
#include <nav_2d_msgs/Twist2D.h>
#include <dwb_msgs/CriticScore.h>
#include <pluginlib/class_list_macros.h>
#include <string>
#include <vector>
#include <algorithm>

namespace dwb_local_planner
{

DWBLocalPlanner::DWBLocalPlanner() :
  traj_gen_loader_("dwb_local_planner", "dwb_local_planner::TrajectoryGenerator"),
  goal_checker_loader_("dwb_local_planner", "dwb_local_planner::GoalChecker"),
  critic_loader_("dwb_local_planner", "dwb_local_planner::TrajectoryCritic")
{
}

void DWBLocalPlanner::initialize(const ros::NodeHandle& parent, const std::string& name,
                                 TFListenerPtr tf, nav_core2::Costmap::Ptr costmap)
{
  tf_ = tf;
  costmap_ = costmap;
  planner_nh_ = ros::NodeHandle(parent, name);

  // This is needed when using the CostmapAdapter to ensure that the costmap's info matches the rolling window
  planner_nh_.param("update_costmap_before_planning", update_costmap_before_planning_, true);

  planner_nh_.param("prune_plan", prune_plan_, true);
  planner_nh_.param("prune_distance", prune_distance_, 1.0);
  planner_nh_.param("short_circuit_trajectory_evaluation", short_circuit_trajectory_evaluation_, true);
  planner_nh_.param("debug_trajectory_details", debug_trajectory_details_, false);
  pub_.initialize(planner_nh_);

  // Plugins
  std::string traj_generator_name;
  planner_nh_.param("trajectory_generator_name", traj_generator_name,
                    getBackwardsCompatibleDefaultGenerator(planner_nh_));
  ROS_INFO_NAMED("DWBLocalPlanner", "Using Trajectory Generator \"%s\"", traj_generator_name.c_str());
  traj_generator_ = std::move(traj_gen_loader_.createUniqueInstance(traj_generator_name));
  traj_generator_->initialize(planner_nh_);

  std::string goal_checker_name;
  planner_nh_.param("goal_checker_name", goal_checker_name, std::string("dwb_plugins::SimpleGoalChecker"));
  ROS_INFO_NAMED("DWBLocalPlanner", "Using Goal Checker \"%s\"", goal_checker_name.c_str());
  goal_checker_ = std::move(goal_checker_loader_.createUniqueInstance(goal_checker_name));
  goal_checker_->initialize(planner_nh_);

  loadCritics(name);
}

std::string DWBLocalPlanner::resolveCriticClassName(std::string base_name)
{
  if (base_name.find("Critic") == std::string::npos)
  {
    base_name = base_name + "Critic";
  }

  if (base_name.find("::") == std::string::npos)
  {
    for (unsigned int j = 0; j < default_critic_namespaces_.size(); j++)
    {
      std::string full_name = default_critic_namespaces_[j] + "::" + base_name;
      if (critic_loader_.isClassAvailable(full_name))
      {
        return full_name;
      }
    }
  }
  return base_name;
}

void DWBLocalPlanner::loadCritics(const std::string name)
{
  planner_nh_.param("default_critic_namespaces", default_critic_namespaces_);
  if (default_critic_namespaces_.size() == 0)
  {
    default_critic_namespaces_.push_back("dwb_critics");
  }

  if (!planner_nh_.hasParam("critics"))
  {
    loadBackwardsCompatibleParameters(planner_nh_);
  }

  std::vector<std::string> critic_names;
  planner_nh_.getParam("critics", critic_names);
  for (unsigned int i = 0; i < critic_names.size(); i++)
  {
    std::string plugin_name = critic_names[i];
    std::string plugin_class;
    planner_nh_.param(plugin_name + "/class", plugin_class, plugin_name);
    plugin_class = resolveCriticClassName(plugin_class);

    TrajectoryCritic::Ptr plugin = std::move(critic_loader_.createUniqueInstance(plugin_class));
    ROS_INFO_NAMED("DWBLocalPlanner", "Using critic \"%s\" (%s)", plugin_name.c_str(), plugin_class.c_str());
    critics_.push_back(plugin);
    plugin->initialize(planner_nh_, plugin_name, costmap_);
  }
}

bool DWBLocalPlanner::isGoalReached(const nav_2d_msgs::Pose2DStamped& pose, const nav_2d_msgs::Twist2D& velocity)
{
  if (goal_pose_.header.frame_id == "")
  {
    ROS_WARN_NAMED("DWBLocalPlanner", "Cannot check if the goal is reached without the goal being set!");
    return false;
  }

  // Update time stamp of goal pose
  goal_pose_.header.stamp = pose.header.stamp;
  intermediate_goal_pose_.header.stamp = pose.header.stamp;

  // use the goal_checker_ to check if the intermediate goal was reached.
  bool ret = goal_checker_->isGoalReached(
              transformPoseToLocal(pose),
              transformPoseToLocal(intermediate_goal_pose_),
              velocity);
  if (ret)
  {
    if (global_plan_segments_.empty())
      ROS_INFO_THROTTLE_NAMED(1.0, "DWBLocalPlanner", "Goal reached!");
    else
    {
      ROS_INFO_THROTTLE_NAMED(1.0, "DWBLocalPlanner", "Intermediate goal reached!");
      ret = false; // reset to false, as we only finished one segment.

      // activate next path segment
      global_plan_ = global_plan_segments_[0];
      global_plan_segments_.erase(global_plan_segments_.begin());
      // set the next goal pose
      intermediate_goal_pose_.header = global_plan_.header;
      intermediate_goal_pose_.pose = global_plan_.poses.back();

      // publish the next path segment
      pub_.publishGlobalPlan(global_plan_);

      // reset critics etc, as we changed the global_plan_
      resetPlugins();
      // prepare(...) will be called soon, automatically, when computing the
      // velocity commands.
    }
  }
  return ret;
}

void DWBLocalPlanner::setGoalPose(const nav_2d_msgs::Pose2DStamped& goal_pose)
{
  ROS_INFO_NAMED("DWBLocalPlanner", "New Goal Received.");
  goal_pose_ = goal_pose;
  intermediate_goal_pose_ = goal_pose; // For now assume that the path will not
                                       // be split. Else the intermediate goal
                                       // will be reset in setPlan.
}

void DWBLocalPlanner::setPlan(const nav_2d_msgs::Path2D& path)
{
  bool split_path;
  planner_nh_.param("split_path", split_path, false);

  global_plan_segments_.clear();

  if (split_path)
  {
    /*
      Global planners like SBPL might create trajectories with complex
      maneuvers, switching between driving forwards and backwards, where it is
      crucial that the individual segments are carefully followed and completed
      before starting the next. The "split_path" option allows to split the
      given path in such segments and to treat each of them independently.

      The segmentation is computed as follows:
      Given two poses in the path, we compute the vector v1 connecting them,
      the vector of orientation given through the angle theta, check the dot
      product of the vectors: If it is less than 0, the angle is over 90 deg,
      which is a coarse approximation for "driving backwards", and for "driving
      forwards" otherwise. In the special case that the vector connecting the
      two poses is null we have to deal with in-place-rotations of the robot.
      To avoid the robot from eagerly driving forward before having achieved the
      correct orientation, these situations are grouped in a third category.
      The path is then split into segments of the same movement-direction
      (forward/backward/onlyRotation). When a cut is made, the last pose of the
      last segment is copied, to be the first pose of the following segment, to
      ensure a seamless path.

      The "global_plan_" variable is then set to the first segment.
      In the "isGoalReached" method, which is repeatedly called, we check if
      the intermediate goal - i.e., the end of the current segment - is reached,
      and if so proceed to the next segment.
    */
    ROS_INFO_NAMED("DWBLocalPlanner", "Splitting path...");

    auto copy = path;
    while (copy.poses.size() > 1) // need at least 2 poses in a path
    {
      // start a new segment
      nav_2d_msgs::Path2D segment;
      segment.header = path.header;

      // add the first pose
      segment.poses.push_back(copy.poses[0]);
      copy.poses.erase(copy.poses.begin());

      // add the second pose and determine if we are going forward or backward
      segment.poses.push_back(copy.poses[0]);
      copy.poses.erase(copy.poses.begin());

      // take the vector from the first to the second position and compare it
      // to the orientation vector at the first position. If the angle is > 90 deg,
      // assume we are driving backwards.
      double v1[2] = {
          segment.poses[1].x - segment.poses[0].x,
          segment.poses[1].y - segment.poses[0].y
      };
      double v2[2] = {
          cos(segment.poses[0].theta),
          sin(segment.poses[0].theta)
      };
      double d = v1[0] * v2[0] + v1[1] * v2[1]; // dotproduct of the vectors. if < 0, the angle is over 90 degrees.
      bool backwards = (d < 0);
      bool onlyRotation = (d*d) < 1e-10; // if the dotproduct is zero, the two positions are equal
                                        // and the plan is to rotate on the spot. Since dwb would
                                        // enthusiastically speed up at the start of a trajectory,
                                        // even if it starts with in-place-rotation, it would
                                        // miss the path by a lot. So lets split the path into
                                        // forwards / backwards / onlyRotation.


      // add more poses while they lead into the same direction (driving forwards/backwards/onlyRotation)
      while (copy.poses.size() > 0)
      {
        // vector from the last pose in the segment to the next in the path
        double v1[2] = {
            copy.poses[0].x - segment.poses.back().x,
            copy.poses[0].y - segment.poses.back().y
        };
        // orientation at the last pose in the segment
        double v2[2] = {
            cos(segment.poses.back().theta),
            sin(segment.poses.back().theta)
        };
        double d = v1[0] * v2[0] + v1[1] * v2[1]; // dotproduct of the vectors. if < 0, the angle is over 90 degrees.
        bool b = (d < 0);
        bool rot = (d*d) < 1e-10;

        if (b == backwards && rot == onlyRotation)
        {
          // same direction -> just add the pose
          segment.poses.push_back(copy.poses[0]);
          copy.poses.erase(copy.poses.begin());
        }
        else
        {
          // direction changes -> add the last pose of the last segment back to
          // the path, to start a new segment at the end of the last
          copy.poses.insert(copy.poses.begin(), segment.poses.back());
          break;
        }
      }

      // add the created segment to the list
      global_plan_segments_.push_back(segment);
    }

    ROS_INFO_STREAM("Split path into " << global_plan_segments_.size() << " segments.");
  }
  else
  {
      // split_path option is disabled, hence there is only one segment, which
      // is the complete path.
      global_plan_segments_.push_back(path);
  }

  // set the global_plan_ to be the first segment
  global_plan_ = global_plan_segments_[0];
  global_plan_segments_.erase(global_plan_segments_.begin());

  // publish not the complete path, but only the first segment.
  pub_.publishGlobalPlan(global_plan_);

  // set the intermediate goal
  intermediate_goal_pose_.header = global_plan_.header;
  intermediate_goal_pose_.pose = global_plan_.poses.back();

  resetPlugins();
  // prepare(...) to initialize the critics for the new segment does not need
  // to be called here, as it is called at every computeVelocityCommands-call.
}

void DWBLocalPlanner::resetPlugins()
{
  traj_generator_->reset();
  goal_checker_->reset();
  for (TrajectoryCritic::Ptr critic : critics_)
  {
    critic->reset();
  }
}

nav_2d_msgs::Twist2DStamped DWBLocalPlanner::computeVelocityCommands(const nav_2d_msgs::Pose2DStamped& pose,
                                                                     const nav_2d_msgs::Twist2D& velocity)
{
  std::shared_ptr<dwb_msgs::LocalPlanEvaluation> results = nullptr;
  if (pub_.shouldRecordEvaluation())
  {
    results = std::make_shared<dwb_msgs::LocalPlanEvaluation>();
  }

  try
  {
    nav_2d_msgs::Twist2DStamped cmd_vel = computeVelocityCommands(pose, velocity, results);
    pub_.publishEvaluation(results);
    return cmd_vel;
  }
  catch (const nav_core2::PlannerException& e)
  {
    pub_.publishEvaluation(results);
    throw;
  }
}

void DWBLocalPlanner::prepare(const nav_2d_msgs::Pose2DStamped& pose, const nav_2d_msgs::Twist2D& velocity)
{
  if (update_costmap_before_planning_)
  {
    costmap_->update();
  }

  nav_2d_msgs::Path2D transformed_plan = transformGlobalPlan(pose);
  pub_.publishTransformedPlan(transformed_plan);

  // Update time stamp of goal pose
  goal_pose_.header.stamp = pose.header.stamp;
  intermediate_goal_pose_.header.stamp = pose.header.stamp;

  geometry_msgs::Pose2D local_start_pose = transformPoseToLocal(pose),
                        local_goal_pose = transformPoseToLocal(intermediate_goal_pose_);

  pub_.publishInputParams(costmap_->getInfo(), local_start_pose, velocity, local_goal_pose);

  for (TrajectoryCritic::Ptr critic : critics_)
  {
    if (!critic->prepare(local_start_pose, velocity, local_goal_pose, transformed_plan))
    {
      ROS_WARN_NAMED("DWBLocalPlanner", "Critic \"%s\" failed to prepare", critic->getName().c_str());
    }
  }
}

nav_2d_msgs::Twist2DStamped DWBLocalPlanner::computeVelocityCommands(const nav_2d_msgs::Pose2DStamped& pose,
    const nav_2d_msgs::Twist2D& velocity, std::shared_ptr<dwb_msgs::LocalPlanEvaluation>& results)
{
  if (results)
  {
    results->header.frame_id = pose.header.frame_id;
    results->header.stamp = ros::Time::now();
  }

  prepare(pose, velocity);

  try
  {
    dwb_msgs::TrajectoryScore best = coreScoringAlgorithm(pose.pose, velocity, results);

    // Return Value
    nav_2d_msgs::Twist2DStamped cmd_vel;
    cmd_vel.header.stamp = ros::Time::now();
    cmd_vel.velocity = best.traj.velocity;

    // debrief stateful critics
    for (TrajectoryCritic::Ptr critic : critics_)
    {
      critic->debrief(cmd_vel.velocity);
    }

    pub_.publishLocalPlan(pose.header, best.traj);
    pub_.publishCostGrid(costmap_, critics_);

    return cmd_vel;
  }
  catch (const NoLegalTrajectoriesException& e)
  {
    nav_2d_msgs::Twist2D empty_cmd;
    dwb_msgs::Trajectory2D empty_traj;
    // debrief stateful scoring functions
    for (TrajectoryCritic::Ptr critic : critics_)
    {
      critic->debrief(empty_cmd);
    }
    pub_.publishLocalPlan(pose.header, empty_traj);
    pub_.publishCostGrid(costmap_, critics_);

    throw;
  }
}

dwb_msgs::TrajectoryScore DWBLocalPlanner::coreScoringAlgorithm(const geometry_msgs::Pose2D& pose,
                                                                const nav_2d_msgs::Twist2D velocity,
                                                                std::shared_ptr<dwb_msgs::LocalPlanEvaluation>& results)
{
  nav_2d_msgs::Twist2D twist;
  dwb_msgs::Trajectory2D traj;
  dwb_msgs::TrajectoryScore best, worst;
  best.total = -1;
  worst.total = -1;
  IllegalTrajectoryTracker tracker;

  traj_generator_->startNewIteration(velocity);
  while (traj_generator_->hasMoreTwists())
  {
    twist = traj_generator_->nextTwist();
    traj = traj_generator_->generateTrajectory(pose, velocity, twist);

    try
    {
      dwb_msgs::TrajectoryScore score = scoreTrajectory(traj, best.total);
      tracker.addLegalTrajectory();
      if (results)
      {
        results->twists.push_back(score);
      }
      if (best.total < 0 || score.total < best.total)
      {
        best = score;
        if (results)
        {
          results->best_index = results->twists.size() - 1;
        }
      }
      if (worst.total < 0 || score.total > worst.total)
      {
        worst = score;
        if (results)
        {
          results->worst_index = results->twists.size() - 1;
        }
      }
    }
    catch (const nav_core2::IllegalTrajectoryException& e)
    {
      if (results)
      {
        dwb_msgs::TrajectoryScore failed_score;
        failed_score.traj = traj;

        dwb_msgs::CriticScore cs;
        cs.name = e.getCriticName();
        cs.raw_score = -1.0;
        failed_score.scores.push_back(cs);
        failed_score.total = -1.0;
        results->twists.push_back(failed_score);
      }
      tracker.addIllegalTrajectory(e);
    }
  }

  if (best.total < 0)
  {
    if (debug_trajectory_details_)
    {
      ROS_ERROR_NAMED("DWBLocalPlanner", "%s", tracker.getMessage().c_str());
      for (auto const& x : tracker.getPercentages())
      {
        ROS_ERROR_NAMED("DWBLocalPlanner", "%.2f: %10s/%s", x.second, x.first.first.c_str(), x.first.second.c_str());
      }
    }
    throw NoLegalTrajectoriesException(tracker);
  }

  return best;
}

dwb_msgs::TrajectoryScore DWBLocalPlanner::scoreTrajectory(const dwb_msgs::Trajectory2D& traj,
                                                           double best_score)
{
  dwb_msgs::TrajectoryScore score;
  score.traj = traj;

  for (TrajectoryCritic::Ptr critic : critics_)
  {
    dwb_msgs::CriticScore cs;
    cs.name = critic->getName();
    cs.scale = critic->getScale();

    if (cs.scale == 0.0)
    {
      score.scores.push_back(cs);
      continue;
    }

    double critic_score = critic->scoreTrajectory(traj);
    cs.raw_score = critic_score;
    score.scores.push_back(cs);
    score.total += critic_score * cs.scale;
    if (short_circuit_trajectory_evaluation_ && best_score > 0 && score.total > best_score)
    {
      // since we keep adding positives, once we are worse than the best, we will stay worse
      break;
    }
  }

  return score;
}

double getSquareDistance(const geometry_msgs::Pose2D& pose_a, const geometry_msgs::Pose2D& pose_b)
{
  double x_diff = pose_a.x - pose_b.x;
  double y_diff = pose_a.y - pose_b.y;
  return x_diff * x_diff + y_diff * y_diff;
}

nav_2d_msgs::Path2D DWBLocalPlanner::transformGlobalPlan(const nav_2d_msgs::Pose2DStamped& pose)
{
  nav_2d_msgs::Path2D transformed_plan;
  if (global_plan_.poses.size() == 0)
  {
    throw nav_core2::PlannerException("Received plan with zero length");
  }

  // let's get the pose of the robot in the frame of the plan
  nav_2d_msgs::Pose2DStamped robot_pose;
  if (!nav_2d_utils::transformPose(tf_, global_plan_.header.frame_id, pose, robot_pose))
  {
    throw nav_core2::PlannerTFException("Unable to transform robot pose into global plan's frame");
  }

  transformed_plan.header.frame_id = costmap_->getFrameId();
  transformed_plan.header.stamp = pose.header.stamp;

  // we'll discard points on the plan that are outside the local costmap
  double dist_threshold = std::max(costmap_->getWidth(), costmap_->getHeight()) * costmap_->getResolution() / 2.0;
  double sq_dist_threshold = dist_threshold * dist_threshold;
  nav_2d_msgs::Pose2DStamped stamped_pose;
  stamped_pose.header.frame_id = global_plan_.header.frame_id;

  for (unsigned int i = 0; i < global_plan_.poses.size(); i++)
  {
    bool should_break = false;
    if (getSquareDistance(robot_pose.pose, global_plan_.poses[i]) > sq_dist_threshold)
    {
      if (transformed_plan.poses.size() == 0)
      {
        // we need to skip to a point on the plan that is within a certain distance of the robot
        continue;
      }
      else
      {
        // we're done transforming points
        should_break = true;
      }
    }

    // now we'll transform until points are outside of our distance threshold
    stamped_pose.pose = global_plan_.poses[i];
    transformed_plan.poses.push_back(transformPoseToLocal(stamped_pose));
    if (should_break) break;
  }

  // Prune both plans based on robot position
  // Note that this part of the algorithm assumes that the global plan starts near the robot (at one point)
  // Otherwise it may take a few iterations to converge to the proper behavior
  if (prune_plan_)
  {
    // let's get the pose of the robot in the frame of the transformed_plan/costmap
    nav_2d_msgs::Pose2DStamped costmap_pose;
    if (!nav_2d_utils::transformPose(tf_, transformed_plan.header.frame_id, pose, costmap_pose))
    {
      throw nav_core2::PlannerTFException("Unable to transform robot pose into costmap's frame");
    }

    ROS_ASSERT(global_plan_.poses.size() >= transformed_plan.poses.size());
    std::vector<geometry_msgs::Pose2D>::iterator it = transformed_plan.poses.begin();
    std::vector<geometry_msgs::Pose2D>::iterator global_it = global_plan_.poses.begin();
    double sq_prune_dist = prune_distance_ * prune_distance_;
    while (it != transformed_plan.poses.end())
    {
      const geometry_msgs::Pose2D& w = *it;
      // Fixed error bound of 1 meter for now. Can reduce to a portion of the map size or based on the resolution
      if (getSquareDistance(costmap_pose.pose, w) < sq_prune_dist)
      {
        ROS_DEBUG_NAMED("DWBLocalPlanner", "Nearest waypoint to <%f, %f> is <%f, %f>\n",
                                           costmap_pose.pose.x, costmap_pose.pose.y, w.x, w.y);
        break;
      }
      it = transformed_plan.poses.erase(it);
      global_it = global_plan_.poses.erase(global_it);
    }
    pub_.publishGlobalPlan(global_plan_);
  }

  if (transformed_plan.poses.size() == 0)
  {
    throw nav_core2::PlannerException("Resulting plan has 0 poses in it.");
  }
  return transformed_plan;
}

geometry_msgs::Pose2D DWBLocalPlanner::transformPoseToLocal(const nav_2d_msgs::Pose2DStamped& pose)
{
  return nav_2d_utils::transformStampedPose(tf_, pose, costmap_->getFrameId());
}

}  // namespace dwb_local_planner


//  register this planner as a LocalPlanner plugin
PLUGINLIB_EXPORT_CLASS(dwb_local_planner::DWBLocalPlanner, nav_core2::LocalPlanner)
