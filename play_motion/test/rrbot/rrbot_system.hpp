#ifndef PLAY_MOTION__RRBOT_SYSTEM_HPP_
#define PLAY_MOTION__RRBOT_SYSTEM_HPP_

#include <memory>
#include <string>
#include <vector>

#include "hardware_interface/base_interface.hpp"
#include "hardware_interface/system_interface.hpp"
#include "hardware_interface/handle.hpp"
#include "hardware_interface/hardware_info.hpp"
#include "hardware_interface/types/hardware_interface_return_values.hpp"
#include "hardware_interface/types/hardware_interface_status_values.hpp"
#include "play_motion/visibility_control.h"
#include "rclcpp/macros.hpp"

namespace play_motion
{
class RRBotSystem : public
  hardware_interface::BaseInterface<hardware_interface::SystemInterface>
{
public:
  RCLCPP_SHARED_PTR_DEFINITIONS(RRBotSystem);

  PLAY_MOTION_PUBLIC
  hardware_interface::return_type configure(const hardware_interface::HardwareInfo & info) override;

  PLAY_MOTION_PUBLIC
  std::vector<hardware_interface::StateInterface> export_state_interfaces() override;

  PLAY_MOTION_PUBLIC
  std::vector<hardware_interface::CommandInterface> export_command_interfaces() override;

  PLAY_MOTION_PUBLIC
  hardware_interface::return_type start() override;

  PLAY_MOTION_PUBLIC
  hardware_interface::return_type stop() override;

  PLAY_MOTION_PUBLIC
  hardware_interface::return_type read() override;

  PLAY_MOTION_PUBLIC
  hardware_interface::return_type write() override;

private:
  std::vector<double> position_commands_;
  std::vector<double> position_states_;
  std::vector<double> velocity_states_;
};

}  // namespace play_motion

#endif  // PLAY_MOTION__RRBOT_SYSTEM_HPP_
