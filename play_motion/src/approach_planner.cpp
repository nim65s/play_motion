// Copyright 2021 PAL Robotics S.L.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/** \author Adolfo Rodriguez Tsouroukdissian. */

#include <algorithm>
#include <cassert>
#include <cmath>
#include <memory>
#include <sstream>
#include <string>

#include "trajectory_msgs/msg/joint_trajectory.hpp"
#include "moveit/move_group_interface/move_group_interface.h"

#include "rclcpp/exceptions.hpp"
#include "rclcpp/logging.hpp"

#include "play_motion/approach_planner.hpp"

namespace
{

/// \return Comma-separated list of container elements.
template<class T>
std::string enumerateElementsStr(const T & val)
{
  std::stringstream ss;
  std::copy(val.begin(), val.end(), std::ostream_iterator<typename T::value_type>(ss, ", "));
  std::string ret = ss.str();
  if (!ret.empty()) {ret.erase(ret.size() - 2);} // Remove last ", "
  return ret;
}

using MoveGroupInterface = moveit::planning_interface::MoveGroupInterface;
using MoveGroupInterfacePtr = std::shared_ptr<MoveGroupInterface>;

/// \return Comma-separated list of planning groups.
std::string enumeratePlanningGroups(const std::vector<MoveGroupInterfacePtr> & move_groups)
{
  std::string ret;
  for (auto group : move_groups) {
    ret += group->getName() + ", ";
  }
  if (!ret.empty()) {ret.erase(ret.size() - 2);} // Remove last ", "
  return ret;
}

} // namespace

namespace play_motion
{

ApproachPlanner::PlanningData::PlanningData(MoveGroupInterfacePtr move_group_ptr)
: move_group(move_group_ptr),
  sorted_joint_names(move_group_ptr->getActiveJoints())
{
  std::sort(sorted_joint_names.begin(), sorted_joint_names.end());
}

ApproachPlanner::ApproachPlanner(const rclcpp::Node::SharedPtr & node)
: node_(node),
  logger_(node->get_logger().get_child("approach_planner")),
  joint_tol_(1e-3),
  skip_planning_vel_(0.5),
  skip_planning_min_dur_(0.0),
  planning_disabled_(false)
{
  const std::string JOINT_TOL_STR = "approach_planner.joint_tolerance";
  const std::string PLANNING_GROUPS_STR = "approach_planner.planning_groups";
  const std::string NO_PLANNING_JOINTS_STR = "approach_planner.exclude_from_planning_joints";
  const std::string SKIP_PLANNING_VEL_STR = "approach_planner.skip_planning_approach_vel";
  const std::string SKIP_PLANNING_MIN_DUR_STR = "approach_planner.skip_planning_approach_min_dur";

  // Velocity used in non-planned approaches
  if (node_->has_parameter(SKIP_PLANNING_VEL_STR)) {
    skip_planning_vel_ = node_->get_parameter(SKIP_PLANNING_VEL_STR).as_double();
    RCLCPP_DEBUG_STREAM(
      logger_,
      "Using a max velocity of " << skip_planning_vel_ <<
        " for unplanned approaches.");
  } else {
    RCLCPP_DEBUG_STREAM(
      logger_,
      "Max velocity for unplanned approaches not specified. Using default value of " <<
        skip_planning_vel_);
  }

  // Minimum duration used in non-planned approaches
  if (node_->has_parameter(SKIP_PLANNING_MIN_DUR_STR)) {
    skip_planning_min_dur_ = node_->get_parameter(SKIP_PLANNING_MIN_DUR_STR).as_double();
    RCLCPP_DEBUG_STREAM(
      logger_,
      "Using a min duration of " << skip_planning_min_dur_ <<
        " for unplanned approaches.");
  } else {
    RCLCPP_DEBUG_STREAM(
      logger_,
      "Min duration for unplanned approaches not specified. Using default value of " <<
        skip_planning_min_dur_);
  }

  // Initialize motion planning capability, unless explicitly disabled
  if (node->has_parameter("disable_motion_planning")) {
    planning_disabled_ = node->get_parameter("disable_motion_planning").as_bool();
  }
  if (planning_disabled_) {
    RCLCPP_WARN_STREAM(
      logger_,
      "Motion planning capability disabled. Goals requesting planning (the default) will be rejected.\n" <<
        "To disable planning in goal requests set 'skip_planning=true'");
    return;     // Skip initialization of planning-related members
  }

  // Joint tolerance
  if (node_->has_parameter(JOINT_TOL_STR)) {
    joint_tol_ = node_->get_parameter(JOINT_TOL_STR).as_double();
    RCLCPP_DEBUG_STREAM(logger_, "Using joint tolerance of " << joint_tol_);
  } else {
    RCLCPP_DEBUG_STREAM(
      logger_,
      "Joint tolerance not specified. Using default value of " << joint_tol_);
  }

  // Joints excluded from motion planning
  if (node_->has_parameter(NO_PLANNING_JOINTS_STR)) {
    no_plan_joints_ = node_->get_parameter(NO_PLANNING_JOINTS_STR).as_string_array();
  }

  // Planning group names
  if (!node_->has_parameter(PLANNING_GROUPS_STR)) {
    const std::string what =
      "Unspecified planning groups for computing approach trajectories. Please set the '" +
      PLANNING_GROUPS_STR + "' parameter";
    throw std::runtime_error(what);
  }

  std::vector<std::string> planning_groups =
    node_->get_parameter(PLANNING_GROUPS_STR).as_string_array();

  /// @todo ros2: is this even necessary anymore?
  // Move group instances require their own spinner thread. To isolate this asynchronous spinner from the rest of the
  // node, it is set up in a node handle with a custom callback queue
  //  ros::NodeHandle as_nh;
  //  cb_queue_.reset(new ros::CallbackQueue());
  //  as_nh.setCallbackQueue(cb_queue_.get());
  //  spinner_.reset(new ros::AsyncSpinner(1, cb_queue_.get()));
  //  spinner_->start();

  // Populate planning data
  for (const auto & planning_group : planning_groups) {
    MoveGroupInterface::Options opts(planning_group);
    MoveGroupInterfacePtr move_group(new MoveGroupInterface(node_, opts));   // TODO: Timeout and retry, log feedback. Throw on failure
    planning_data_.push_back(PlanningData(move_group));
  }
}

// TODO: Work directly with JointStates and JointTrajector messages?
bool ApproachPlanner::prependApproach(
  const JointNames & joint_names,
  const std::vector<double> & current_pos,
  bool skip_planning,
  const std::vector<TrajPoint> & traj_in,
  std::vector<TrajPoint> & traj_out)
{
  // TODO: Instead of returning false, raise exceptions, so error message can be forwarded to goal result

  // Empty trajectory. Nothing to do
  if (traj_in.empty()) {
    RCLCPP_DEBUG_STREAM(logger_, "Approach motion not needed: Input trajectory is empty.");
    traj_out = traj_in;
    return true;
  }

  const unsigned int joint_dim = traj_in.front().positions.size();

  // Preconditions
  if (joint_dim != joint_names.size()) {
    RCLCPP_ERROR_STREAM(
      logger_,
      "Can't compute approach trajectory: Size mismatch between joint names and input trajectory.");
    return false;
  }
  if (joint_dim != current_pos.size()) {
    RCLCPP_ERROR_STREAM(
      logger_,
      "Can't compute approach trajectory: Size mismatch between current joint positions and input trajectory.");
    return false;
  }
  if (!skip_planning && planning_disabled_) { // Reject goal if plannign is disabled, but goal requests it
    RCLCPP_ERROR_STREAM(
      logger_,
      "Motion planning capability disabled. To disable planning in goal requests, set 'skip_planning=true'");
    return false;
  }

  if (skip_planning) {
    // Skip motion planning altogether
    traj_out = traj_in;

    // If the first waypoint specifies zero time from start, set a duration that does not exceed a specified
    // max avg velocity
    if (rclcpp::Duration(traj_out.front().time_from_start).nanoseconds() == 0) {
      const double reach_time = noPlanningReachTime(current_pos, traj_out.front().positions);
      const rclcpp::Duration reach_time_duration = rclcpp::Duration::from_seconds(reach_time);
      for (auto & point : traj_out) {
        point.time_from_start = rclcpp::Duration(point.time_from_start) + reach_time_duration;
      }
    }
  } else {
    // Compute approach trajectory using motion planning
    trajectory_msgs::msg::JointTrajectory approach;
    const bool approach_ok = computeApproach(
      joint_names,
      current_pos,
      traj_in.front().positions,
      approach);
    if (!approach_ok) {return false;}

    // No approach is required
    if (approach.points.empty()) {
      traj_out = traj_in;
      RCLCPP_INFO_STREAM(logger_, "Approach motion not needed.");
    } else {
      // Combine approach and input motion trajectories
      combineTrajectories(
        joint_names,
        current_pos,
        traj_in,
        approach,
        traj_out);
    }
  }

  // Deal with first waypoints specifying zero time from start. Two cases can happen:
  // 1. If at least one joint is not at its destination, compute an appropriate reach time
  const double eps_time = 1e-3; // NOTE: Magic number
  if (rclcpp::Duration(traj_out.front().time_from_start).nanoseconds() == 0) {
    const double reach_time = noPlanningReachTime(current_pos, traj_out.front().positions);
    const rclcpp::Duration reach_time_duration = rclcpp::Duration::from_seconds(reach_time);
    if (reach_time > eps_time) {
      for (auto & point : traj_out) {
        point.time_from_start = rclcpp::Duration(point.time_from_start) + reach_time_duration;
      }
    }
  }
  // 2 . First waypoint corresponds to current state: Make the first time_from_start a small nonzero value.
  // Rationale: Sending a waypoint with zero time from start will make the controllers complain with a warning, and
  // rightly so, because in general it's impossible to reach a point in zero time.
  // This avoids unsavory warnings that might confuse users.
  if (rclcpp::Duration(traj_out.front().time_from_start).nanoseconds() == 0) { // If still zero it's because previous step yield zero time
    traj_out.front().time_from_start = rclcpp::Duration::from_seconds(eps_time);
  }

  return true;
}

bool ApproachPlanner::needsApproach(
  const std::vector<double> & current_pos,
  const std::vector<double> & goal_pos)
{
  assert(current_pos.size() == goal_pos.size());
  for (unsigned int i = 0; i < current_pos.size(); ++i) {
    if (std::abs(current_pos[i] - goal_pos[i]) > joint_tol_) {return true;}
  }
  return false;
}

bool ApproachPlanner::computeApproach(
  const std::vector<std::string> & joint_names,
  const std::vector<double> & current_pos,
  const std::vector<double> & goal_pos,
  trajectory_msgs::msg::JointTrajectory & traj)
{
  traj.joint_names.clear();
  traj.points.clear();

  // Maximum set of joints that a planning group can have. Corresponds to the original motion joints minus the joints
  // excluded from planning. Planning groups eligible to compute the approach can't contain joints outside this set.
  JointNames max_planning_group;

  // Joint positions associated to the maximum set
  std::vector<double> max_planning_values;

  // Minimum set of joints that a planning group can have. Corresponds to the maximum set minus the joints that are
  // already at their goal configuration. If this set is empty, no approach is required, i.e. all motion joints are
  // either excluded from planning or already at the goal.
  JointNames min_planning_group;

  for (unsigned int i = 0; i < joint_names.size(); ++i) {
    if (isPlanningJoint(joint_names[i])) {
      max_planning_group.push_back(joint_names[i]);
      max_planning_values.push_back(goal_pos[i]);
      if (std::abs(current_pos[i] - goal_pos[i]) > joint_tol_) {
        min_planning_group.push_back(joint_names[i]);
      }
    }
  }

  // No planning is required, return empty trajectory
  if (min_planning_group.empty()) {return true;}

  // Find planning groups that are eligible for computing this particular approach trajectory
  std::vector<MoveGroupInterfacePtr> valid_move_groups = getValidMoveGroups(
    min_planning_group,
    max_planning_group);
  if (valid_move_groups.empty()) {
    RCLCPP_ERROR_STREAM(
      logger_, "Can't compute approach trajectory. There are no planning groups that span at least these joints:" <<
        "\n[" << enumerateElementsStr(
        min_planning_group) << "]\n" << "and at most these joints:" <<
        "\n[" << enumerateElementsStr(max_planning_group) << "].");
    return false;
  } else {
    RCLCPP_INFO_STREAM(
      logger_, "Approach motion can be computed by the following groups: " <<
        enumeratePlanningGroups(valid_move_groups) << ".");
  }

  // Call motion planners
  bool approach_ok = false;
  for (auto & move_group : valid_move_groups) {
    approach_ok = planApproach(max_planning_group, max_planning_values, move_group, traj);
    if (approach_ok) {break;}
  }

  if (!approach_ok) {
    RCLCPP_ERROR_STREAM(
      logger_, "Failed to compute approach trajectory with planning groups: [" <<
        enumeratePlanningGroups(valid_move_groups) << "].");
    return false;
  }

  return true;
}

bool ApproachPlanner::planApproach(
  const JointNames & joint_names,
  const std::vector<double> & joint_values,
  MoveGroupInterfacePtr move_group,
  trajectory_msgs::msg::JointTrajectory & traj)
{
  move_group->setStartStateToCurrentState();
  for (unsigned int i = 0; i < joint_names.size(); ++i) {
    const bool set_goal_ok = move_group->setJointValueTarget(joint_names[i], joint_values[i]);
    if (!set_goal_ok) {
      RCLCPP_ERROR_STREAM(
        logger_, "Failed attempt to set planning goal for joint '" << joint_names[i] << "' on group '" <<
          move_group->getName() << "'.");
      return false;
    }
  }
  moveit::planning_interface::MoveGroupInterface::Plan plan;
  const moveit::planning_interface::MoveItErrorCode planning_ok = move_group->plan(plan);
  if (!(planning_ok == moveit::planning_interface::MoveItErrorCode::SUCCESS)) {
    RCLCPP_DEBUG_STREAM(
      logger_,
      "Could not compute approach trajectory with planning group '" << move_group->getName() <<
        "'.");
    return false;
  }
  if (plan.trajectory_.joint_trajectory.points.empty()) {
    RCLCPP_ERROR_STREAM(
      logger_,
      "Unexpected error: Approach trajectory computed by group '" << move_group->getName() <<
        "' is empty.");
    return false;
  }

  traj = plan.trajectory_.joint_trajectory;
  RCLCPP_INFO_STREAM(
    logger_,
    "Successfully computed approach with planning group '" << move_group->getName() << "'.");
  return true;
}

void ApproachPlanner::combineTrajectories(
  const JointNames & joint_names,
  const std::vector<double> & current_pos,
  const std::vector<TrajPoint> & traj_in,
  trajectory_msgs::msg::JointTrajectory & approach,
  std::vector<TrajPoint> & traj_out)
{
  const unsigned int joint_dim = traj_in.front().positions.size();

  for (const auto & point_appr : approach.points) {
    const bool has_velocities = !point_appr.velocities.empty();
    const bool has_accelerations = !point_appr.accelerations.empty();
    TrajPoint point;
    point.positions.resize(joint_dim, 0.0);
    if (has_velocities) {point.velocities.resize(joint_dim, 0.0);}
    if (has_accelerations) {point.accelerations.resize(joint_dim, 0.0);}
    point.time_from_start = point_appr.time_from_start;

    for (unsigned int i = 0; i < joint_dim; ++i) {
      const JointNames & plan_joints = approach.joint_names;
      JointNames::const_iterator approach_joints_it = find(
        plan_joints.begin(),
        plan_joints.end(), joint_names[i]);
      if (approach_joints_it != plan_joints.end()) {
        // Joint is part of the planned approach
        const unsigned int approach_id = std::distance(plan_joints.begin(), approach_joints_it);
        point.positions[i] = point_appr.positions[approach_id];
        if (has_velocities) {point.velocities[i] = point_appr.velocities[approach_id];}
        if (has_accelerations) {point.accelerations[i] = point_appr.accelerations[approach_id];}
      } else {
        // Joint is not part of the planning group, and hence not contained in the approach plan
        // Default to linear interpolation TODO: Use spline interpolator
        const double t_min = 0.0;
        const double t_max = rclcpp::Duration(approach.points.back().time_from_start).seconds();
        const double t = rclcpp::Duration(point_appr.time_from_start).seconds();

        const double p_min = current_pos[i];
        const double p_max = traj_in.front().positions[i];

        const double vel = (p_max - p_min) / (t_max - t_min);

        point.positions[i] = p_min + vel * t;
        if (has_velocities) {point.velocities[i] = vel;}
        if (has_accelerations) {point.accelerations[i] = 0.0;}
      }
    }

    traj_out.push_back(point);
  }

  // If input trajectory is a single point, the approach trajectory is all there is to execute...
  if (1 == traj_in.size()) {return;}

  // ...otherwise, append input_trajectory after approach:

  // Time offset to apply to input trajectory (approach duration)
  const rclcpp::Duration offset(traj_out.back().time_from_start);

  // Remove duplicate waypoint: Position of last approach point coincides with the input's first point
  traj_out.pop_back();

  // Append input trajectory to approach
  for (const auto & point : traj_in) {
    traj_out.push_back(point);
    traj_out.back().time_from_start = rclcpp::Duration(traj_out.back().time_from_start) + offset;
  }
}

std::vector<ApproachPlanner::MoveGroupInterfacePtr> ApproachPlanner::getValidMoveGroups(
  const JointNames & min_group,
  const JointNames & max_group)
{
  std::vector<MoveGroupInterfacePtr> valid_groups;

  // Create sorted ranges of min/max planning groups
  JointNames min_group_s = min_group;
  JointNames max_group_s = max_group;
  std::sort(min_group_s.begin(), min_group_s.end());
  std::sort(max_group_s.begin(), max_group_s.end());

  for (const auto & data : planning_data_) {
    const JointNames & group_s = data.sorted_joint_names;

    // A valid planning group is one that has the minimum group as a subset, and is a subset of the maximum group
    if (std::includes(group_s.begin(), group_s.end(), min_group_s.begin(), min_group_s.end()) &&
      std::includes(max_group_s.begin(), max_group_s.end(), group_s.begin(), group_s.end()))
    {
      valid_groups.push_back(data.move_group);
    }
  }
  return valid_groups;
}

bool ApproachPlanner::isPlanningJoint(const std::string & joint_name) const
{
  return std::find(
    no_plan_joints_.begin(), no_plan_joints_.end(),
    joint_name) == no_plan_joints_.end();
}

double ApproachPlanner::noPlanningReachTime(
  const std::vector<double> & curr_pos,
  const std::vector<double> & goal_pos)
{
  double dmax = 0.0; // Maximum joint displacement
  for (unsigned int i = 0; i < curr_pos.size(); ++i) {
    const double d = std::abs(goal_pos[i] - curr_pos[i]);
    if (d > dmax) {
      dmax = d;
    }
  }
  return std::max(dmax / skip_planning_vel_, skip_planning_min_dur_);
}

} // namesapce
