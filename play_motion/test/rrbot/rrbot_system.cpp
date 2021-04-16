// Copyright 2020 ros2_control Development Team
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

#include "rrbot_system.hpp"

#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include "hardware_interface/types/hardware_interface_type_values.hpp"
#include "rclcpp/rclcpp.hpp"

namespace play_motion
{

hardware_interface::return_type RRBotSystem::configure(
  const hardware_interface::HardwareInfo & info)
{
  if (configure_default(info) != hardware_interface::return_type::OK) {
    return hardware_interface::return_type::ERROR;
  }

  position_states_.resize(info_.joints.size(), std::numeric_limits<double>::quiet_NaN());
  velocity_states_.resize(info_.joints.size(), std::numeric_limits<double>::quiet_NaN());
  position_commands_.resize(info_.joints.size(), std::numeric_limits<double>::quiet_NaN());

  for (const hardware_interface::ComponentInfo & joint : info_.joints) {
    // rrbot_system has exactly one state and command interface on each joint
    if (joint.command_interfaces.size() != 1) {
      RCLCPP_FATAL(
        rclcpp::get_logger("rrbot_system"),
        "Joint '%s' has %d command interfaces found. 1 expected.",
        joint.name.c_str(), joint.command_interfaces.size());
      return hardware_interface::return_type::ERROR;
    }

    if (joint.command_interfaces[0].name != hardware_interface::HW_IF_POSITION) {
      RCLCPP_FATAL(
        rclcpp::get_logger("rrbot_system"),
        "Joint '%s' have %s command interfaces found. '%s' expected.",
        joint.name.c_str(), joint.command_interfaces[0].name.c_str(),
        hardware_interface::HW_IF_POSITION);
      return hardware_interface::return_type::ERROR;
    }

    if (joint.state_interfaces.size() != 1) {
      RCLCPP_FATAL(
        rclcpp::get_logger("rrbot_system"),
        "Joint '%s' has %d state interface. 1 expected.",
        joint.name.c_str(), joint.state_interfaces.size());
      return hardware_interface::return_type::ERROR;
    }

    if (joint.state_interfaces[0].name != hardware_interface::HW_IF_POSITION) {
      RCLCPP_FATAL(
        rclcpp::get_logger("rrbot_system"),
        "Joint '%s' have %s state interface. '%s' expected.",
        joint.name.c_str(), joint.state_interfaces[0].name.c_str(),
        hardware_interface::HW_IF_POSITION);
      return hardware_interface::return_type::ERROR;
    }
  }

  status_ = hardware_interface::status::CONFIGURED;
  RCLCPP_INFO_STREAM(rclcpp::get_logger("rrbot_system"), "rrbot_system configured");
  return hardware_interface::return_type::OK;
}

std::vector<hardware_interface::StateInterface>
RRBotSystem::export_state_interfaces()
{
  std::vector<hardware_interface::StateInterface> state_interfaces;
  for (uint i = 0; i < info_.joints.size(); i++) {
    state_interfaces.emplace_back(
      hardware_interface::StateInterface(
        info_.joints[i].name, hardware_interface::HW_IF_POSITION, &position_states_[i]));

    state_interfaces.emplace_back(
      hardware_interface::StateInterface(
        info_.joints[i].name, hardware_interface::HW_IF_VELOCITY, &velocity_states_[i]));
  }
  return state_interfaces;
}

std::vector<hardware_interface::CommandInterface>
RRBotSystem::export_command_interfaces()
{
  std::vector<hardware_interface::CommandInterface> command_interfaces;
  for (uint i = 0; i < info_.joints.size(); i++) {
    command_interfaces.emplace_back(
      hardware_interface::CommandInterface(
        info_.joints[i].name, hardware_interface::HW_IF_POSITION, &position_commands_[i]));
  }
  return command_interfaces;
}

hardware_interface::return_type RRBotSystem::start()
{
  // set some default values when starting the first time
  for (uint i = 0; i < position_states_.size(); i++) {
    if (std::isnan(position_states_[i])) {
      position_states_[i] = 0;
      velocity_states_[i] = 0;
      position_commands_[i] = 0;
    } else {
      position_commands_[i] = position_states_[i];
    }
  }

  status_ = hardware_interface::status::STARTED;
  RCLCPP_INFO_STREAM(rclcpp::get_logger("rrbot_system"), "rrbot_system started");
  return hardware_interface::return_type::OK;
}

hardware_interface::return_type RRBotSystem::stop()
{
  status_ = hardware_interface::status::STOPPED;
  RCLCPP_INFO_STREAM(rclcpp::get_logger("rrbot_system"), "rrbot_system stopped");
  return hardware_interface::return_type::OK;
}

hardware_interface::return_type RRBotSystem::read()
{
  for (uint i = 0; i < position_states_.size(); i++) {
    position_states_[i] = position_commands_[i];

    /// @todo emulate some velocity here?
    velocity_states_[i] = 0.0;
  }
  return hardware_interface::return_type::OK;
}

hardware_interface::return_type RRBotSystem::write()
{
  return hardware_interface::return_type::OK;
}

}  // namespace play_motion

#include "pluginlib/class_list_macros.hpp"

PLUGINLIB_EXPORT_CLASS(play_motion::RRBotSystem, hardware_interface::SystemInterface)
