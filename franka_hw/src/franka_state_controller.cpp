#include <franka_hw/franka_state_controller.h>

#include <cmath>
#include <mutex>
#include <string>

#include <controller_interface/controller_base.h>
#include <hardware_interface/hardware_interface.h>
#include <pluginlib/class_list_macros.h>
#include <ros/ros.h>
#include <tf/tf.h>
#include <tf/transform_datatypes.h>
#include <xmlrpcpp/XmlRpcValue.h>

#include <franka_hw/Errors.h>
#include <franka_hw/franka_cartesian_command_interface.h>

namespace {
tf::Transform convertArrayToTf(const std::array<double, 16>& transform) {
  tf::Matrix3x3 rotation(transform[0], transform[4], transform[8], transform[1], transform[5],
                         transform[9], transform[2], transform[6], transform[10]);
  tf::Vector3 translation(transform[12], transform[13], transform[14]);
  return tf::Transform(rotation, translation);
}

franka_hw::Errors errorsToMessage(const franka::Errors& error) {
  franka_hw::Errors message;
  message.cartesian_motion_generator_acceleration_discontinuity =
      error.cartesian_motion_generator_acceleration_discontinuity;
  message.cartesian_motion_generator_elbow_limit_violation =
      error.cartesian_motion_generator_elbow_limit_violation;
  message.cartesian_motion_generator_elbow_sign_inconsistent =
      error.cartesian_motion_generator_elbow_sign_inconsistent;
  message.cartesian_motion_generator_start_elbow_invalid =
      error.cartesian_motion_generator_start_elbow_invalid;
  message.cartesian_motion_generator_velocity_discontinuity =
      error.cartesian_motion_generator_velocity_discontinuity;
  message.cartesian_motion_generator_velocity_limits_violation =
      error.cartesian_motion_generator_velocity_limits_violation;
  message.cartesian_position_limits_violation = error.cartesian_position_limits_violation;
  message.cartesian_position_motion_generator_start_pose_invalid =
      error.cartesian_position_motion_generator_start_pose_invalid;
  message.cartesian_reflex = error.cartesian_reflex;
  message.cartesian_velocity_profile_safety_violation =
      error.cartesian_velocity_profile_safety_violation;
  message.cartesian_velocity_violation = error.cartesian_velocity_violation;
  message.force_controller_desired_force_tolerance_violation =
      error.force_controller_desired_force_tolerance_violation;
  message.force_control_safety_violation = error.force_control_safety_violation;
  message.joint_motion_generator_acceleration_discontinuity =
      error.joint_motion_generator_acceleration_discontinuity;
  message.joint_motion_generator_position_limits_violation =
      error.joint_motion_generator_position_limits_violation;
  message.joint_motion_generator_velocity_discontinuity =
      error.joint_motion_generator_velocity_discontinuity;
  message.joint_motion_generator_velocity_limits_violation =
      message.joint_motion_generator_velocity_limits_violation;
  message.joint_position_limits_violation = error.joint_position_limits_violation;
  message.joint_position_motion_generator_start_pose_invalid =
      error.joint_position_motion_generator_start_pose_invalid;
  message.joint_reflex = error.joint_reflex;
  message.joint_velocity_violation = error.joint_velocity_violation;
  message.max_goal_pose_deviation_violation = error.max_goal_pose_deviation_violation;
  message.max_path_pose_deviation_violation = error.max_path_pose_deviation_violation;
  message.self_collision_avoidance_violation = error.self_collision_avoidance_violation;
  return message;
}

}  // anonymous namespace

namespace franka_hw {

FrankaStateController::FrankaStateController()
    : franka_state_interface_(nullptr),
      franka_state_handle_(nullptr),
      publisher_transforms_(),
      publisher_franka_states_(),
      publisher_joint_states_(),
      publisher_external_wrench_(),
      trigger_publish_(30.0) {}

bool FrankaStateController::init(hardware_interface::RobotHW* robot_hardware,
                                 ros::NodeHandle& root_node_handle,
                                 ros::NodeHandle& controller_node_handle) {
  franka_state_interface_ = robot_hardware->get<franka_hw::FrankaStateInterface>();
  if (franka_state_interface_ == nullptr) {
    ROS_ERROR("FrankaStateController: Could not get Franka state interface from hardware");
    return false;
  }
  if (!root_node_handle.getParam("arm_id", arm_id_)) {
    ROS_ERROR("FrankaStateController: Could not get parameter arm_id");
    return false;
  }
  double publish_rate(30.0);
  if (controller_node_handle.getParam("publish_rate", publish_rate)) {
    trigger_publish_ = franka_hw::TriggerRate(publish_rate);
  } else {
    ROS_INFO_STREAM("FrankaStateController: Did not find publish_rate. Using default "
                    << publish_rate << " [Hz].");
  }

  if (!root_node_handle.getParam("joint_names", joint_names_) || joint_names_.size() != 7) {
    ROS_ERROR(
        "FrankaStateController: Invalid or no joint_names parameters provided, aborting "
        "controller init!");
    return false;
  }

  try {
    franka_state_handle_.reset(
        new franka_hw::FrankaStateHandle(franka_state_interface_->getHandle(arm_id_ + "_robot")));
  } catch (const hardware_interface::HardwareInterfaceException& ex) {
    ROS_ERROR_STREAM("FrankaStateController: Exception getting cartesian handle: " << ex.what());
    return false;
  }

  publisher_transforms_.init(root_node_handle, "/tf", 1);
  publisher_franka_states_.init(controller_node_handle, "franka_states", 1);
  publisher_joint_states_.init(controller_node_handle, "joint_states", 1);
  publisher_external_wrench_.init(controller_node_handle, "F_ext", 1);

  {
    std::lock_guard<realtime_tools::RealtimePublisher<sensor_msgs::JointState> > lock(
        publisher_joint_states_);
    publisher_joint_states_.msg_.name.resize(7);
    publisher_joint_states_.msg_.position.resize(robot_state_.q.size());
    publisher_joint_states_.msg_.velocity.resize(robot_state_.dq.size());
    publisher_joint_states_.msg_.effort.resize(robot_state_.tau_J.size());
  }
  {
    std::lock_guard<realtime_tools::RealtimePublisher<tf2_msgs::TFMessage> > lock(
        publisher_transforms_);
    publisher_transforms_.msg_.transforms.resize(2);
    tf::Quaternion quaternion(0.0, 0.0, 0.0, 1.0);
    tf::Vector3 translation(0.0, 0.0, 0.05);
    tf::Transform transform(quaternion, translation);
    tf::StampedTransform trafo(transform, ros::Time::now(), arm_id_ + "_link8", arm_id_ + "_EE");
    geometry_msgs::TransformStamped transform_message;
    transformStampedTFToMsg(trafo, transform_message);
    publisher_transforms_.msg_.transforms[0] = transform_message;
    translation = tf::Vector3(0.0, 0.0, 0.0);
    transform = tf::Transform(quaternion, translation);
    trafo = tf::StampedTransform(transform, ros::Time::now(), arm_id_ + "_EE", arm_id_ + "_K");
    transformStampedTFToMsg(trafo, transform_message);
    publisher_transforms_.msg_.transforms[1] = transform_message;
  }
  {
    std::lock_guard<realtime_tools::RealtimePublisher<geometry_msgs::WrenchStamped> > lock(
        publisher_external_wrench_);
    publisher_external_wrench_.msg_.header.frame_id = arm_id_ + "_K";
    publisher_external_wrench_.msg_.wrench.force.x = 0.0;
    publisher_external_wrench_.msg_.wrench.force.y = 0.0;
    publisher_external_wrench_.msg_.wrench.force.z = 0.0;
    publisher_external_wrench_.msg_.wrench.torque.x = 0.0;
    publisher_external_wrench_.msg_.wrench.torque.y = 0.0;
    publisher_external_wrench_.msg_.wrench.torque.z = 0.0;
  }
  return true;
}

void FrankaStateController::update(const ros::Time& time, const ros::Duration& /*period*/) {
  if (trigger_publish_()) {
    robot_state_ = franka_state_handle_->getRobotState();
    publishFrankaStates(time);
    publishTransforms(time);
    publishExternalWrench(time);
    publishJointStates(time);
    sequence_number_++;
  }
}

void FrankaStateController::publishFrankaStates(const ros::Time& time) {
  if (publisher_franka_states_.trylock()) {
    for (size_t i = 0; i < robot_state_.cartesian_collision.size(); ++i) {
      publisher_franka_states_.msg_.cartesian_collision[i] = robot_state_.cartesian_collision[i];
      publisher_franka_states_.msg_.cartesian_contact[i] = robot_state_.cartesian_contact[i];
      publisher_franka_states_.msg_.K_F_ext_hat_K[i] = robot_state_.K_F_ext_hat_K[i];
      publisher_franka_states_.msg_.O_F_ext_hat_K[i] = robot_state_.O_F_ext_hat_K[i];
    }

    for (size_t i = 0; i < robot_state_.q.size(); ++i) {
      publisher_franka_states_.msg_.q[i] = robot_state_.q[i];
      publisher_franka_states_.msg_.dq[i] = robot_state_.dq[i];
      publisher_franka_states_.msg_.tau_J[i] = robot_state_.tau_J[i];
      publisher_franka_states_.msg_.dtau_J[i] = robot_state_.dtau_J[i];
      publisher_franka_states_.msg_.joint_collision[i] = robot_state_.joint_collision[i];
      publisher_franka_states_.msg_.joint_contact[i] = robot_state_.joint_contact[i];
      publisher_franka_states_.msg_.q_d[i] = robot_state_.q_d[i];
      publisher_franka_states_.msg_.tau_ext_hat_filtered[i] = robot_state_.tau_ext_hat_filtered[i];
    }

    for (size_t i = 0; i < robot_state_.elbow.size(); ++i) {
      publisher_franka_states_.msg_.elbow[i] = robot_state_.elbow[i];
    }

    for (size_t i = 0; i < robot_state_.elbow_d.size(); ++i) {
      publisher_franka_states_.msg_.elbow_d[i] = robot_state_.elbow_d[i];
    }

    for (size_t i = 0; i < 16; ++i) {
      publisher_franka_states_.msg_.O_T_EE[i] = robot_state_.O_T_EE[i];
      publisher_franka_states_.msg_.F_T_EE[i] = robot_state_.F_T_EE[i];
      publisher_franka_states_.msg_.EE_T_K[i] = robot_state_.EE_T_K[i];
      publisher_franka_states_.msg_.O_T_EE_d[i] = robot_state_.O_T_EE_d[i];
    }
    publisher_franka_states_.msg_.m_load = robot_state_.m_load;

    for (size_t i = 0; i < 9; ++i) {
      publisher_franka_states_.msg_.I_load[i] = robot_state_.I_load[i];
    }

    for (size_t i = 0; i < 3; ++i) {
      publisher_franka_states_.msg_.F_x_Cload[i] = robot_state_.F_x_Cload[i];
    }

    publisher_franka_states_.msg_.time = robot_state_.time.s();
    publisher_franka_states_.msg_.current_errors = errorsToMessage(robot_state_.current_errors);
    publisher_franka_states_.msg_.last_motion_errors =
        errorsToMessage(robot_state_.last_motion_errors);

    publisher_franka_states_.msg_.header.seq = sequence_number_;
    publisher_franka_states_.msg_.header.stamp = time;
    publisher_franka_states_.unlockAndPublish();
  }
}

void FrankaStateController::publishJointStates(const ros::Time& time) {
  if (publisher_joint_states_.trylock()) {
    for (size_t i = 0; i < 7; ++i) {
      publisher_joint_states_.msg_.name[i] = joint_names_[i];
      publisher_joint_states_.msg_.position[i] = robot_state_.q[i];
      publisher_joint_states_.msg_.velocity[i] = robot_state_.dq[i];
      publisher_joint_states_.msg_.effort[i] = robot_state_.tau_J[i];
    }
    publisher_joint_states_.msg_.header.stamp = time;
    publisher_joint_states_.msg_.header.seq = sequence_number_;
    publisher_joint_states_.unlockAndPublish();
  }
}

void FrankaStateController::publishTransforms(const ros::Time& time) {
  if (publisher_transforms_.trylock()) {
    tf::StampedTransform trafo(convertArrayToTf(robot_state_.F_T_EE), time, arm_id_ + "_link8",
                               arm_id_ + "_EE");
    geometry_msgs::TransformStamped transform_message;
    transformStampedTFToMsg(trafo, transform_message);
    publisher_transforms_.msg_.transforms[0] = transform_message;
    trafo = tf::StampedTransform(convertArrayToTf(robot_state_.EE_T_K), time, arm_id_ + "_EE",
                                 arm_id_ + "_K");
    transformStampedTFToMsg(trafo, transform_message);
    publisher_transforms_.msg_.transforms[1] = transform_message;
    publisher_transforms_.unlockAndPublish();
  }
}

void FrankaStateController::publishExternalWrench(const ros::Time& time) {
  if (publisher_external_wrench_.trylock()) {
    publisher_external_wrench_.msg_.header.frame_id = arm_id_ + "_K";
    publisher_external_wrench_.msg_.header.stamp = time;
    publisher_external_wrench_.msg_.wrench.force.x = robot_state_.K_F_ext_hat_K[0];
    publisher_external_wrench_.msg_.wrench.force.y = robot_state_.K_F_ext_hat_K[1];
    publisher_external_wrench_.msg_.wrench.force.z = robot_state_.K_F_ext_hat_K[2];
    publisher_external_wrench_.msg_.wrench.torque.x = robot_state_.K_F_ext_hat_K[3];
    publisher_external_wrench_.msg_.wrench.torque.y = robot_state_.K_F_ext_hat_K[4];
    publisher_external_wrench_.msg_.wrench.torque.z = robot_state_.K_F_ext_hat_K[5];
    publisher_external_wrench_.unlockAndPublish();
  }
}

}  // namespace franka_hw

PLUGINLIB_EXPORT_CLASS(franka_hw::FrankaStateController, controller_interface::ControllerBase)
