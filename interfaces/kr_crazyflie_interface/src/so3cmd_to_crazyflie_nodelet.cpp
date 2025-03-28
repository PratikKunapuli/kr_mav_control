// This nodelet is meant to subscribe to so3 commands and then convert them
// to geometry_msgs/Twist which is the type for cmd_vel, the crazyflie control
// topic. The format of this is as follows:
//  linear.y = roll     [-30 to 30 degrees]         (may be negative)
//  linear.x = pitch    [-30 to 30 degrees]         (may be negative)
//  linear.z = thrust   [0 to 60,000]               (motors stiction around 2000)
//  angular.z = yawrate [-200 to 200 degrees/second] (note this is not yaw!)

#include <geometry_msgs/Twist.h>
#include <kr_mav_msgs/SO3Command.h>
#include <nav_msgs/Odometry.h>
#include <nodelet/nodelet.h>
#include <ros/ros.h>
#include <crazyflie_driver/sendPacket.h>  // Service definition for sendPacket
#include <crazyflie_driver/crtpPacket.h>  // Message definition for crtpPacket

#include <Eigen/Geometry>
#include <Eigen/Dense>

// TODO: Remove CLAMP as macro
#define CLAMP(x, min, max) ((x) < (min)) ? (min) : ((x) > (max)) ? (max) : (x)

class SO3CmdToCrazyflie : public nodelet::Nodelet
{
 public:
  void onInit(void);

  EIGEN_MAKE_ALIGNED_OPERATOR_NEW

 private:
  void send_arming_request(bool arm);
  void reboot();
  void so3_cmd_callback(const kr_mav_msgs::SO3Command::ConstPtr &msg);
  void odom_callback(const nav_msgs::Odometry::ConstPtr &odom);

  bool odom_set_, so3_cmd_set_;
  ros::Time last_odom_time_;
  Eigen::Quaterniond q_odom_;
  Eigen::Vector3d w_odom_;
  Eigen::Vector3d w_odom_last;
  Eigen::Vector3d w_dot_odom_;

  ros::Publisher attitude_raw_pub_;
  ros::Publisher crazy_fast_cmd_vel_pub_, crazy_cmd_vel_pub_;

  ros::Subscriber so3_cmd_sub_;
  ros::Subscriber odom_sub_;

  ros::ServiceClient packet_client_;  // Service client to send packets

  double so3_cmd_timeout_;
  ros::Time last_so3_cmd_time_;
  kr_mav_msgs::SO3Command last_so3_cmd_;

  double c1_;
  double c2_;
  double c3_;
  double ang_acc_d_gain;

  // TODO get rid of this for the gains coming in
  double kp_yaw_rate_;

  int thrust_pwm_min_;  // Necessary to overcome stiction
  int thrust_pwm_max_;  // Mapped to PWM

  bool send_ctbr_cmds_;
  bool is_brushless_;

  int motor_status_;
  bool armed_;
  int arm_status_;
};

void SO3CmdToCrazyflie::send_arming_request(bool arm)
{
  ROS_INFO("Setting arm to: %d", arm);
  
  // Create a custom packet for arming/disarming
  // https://www.bitcraze.io/documentation/repository/crazyflie-firmware/master/functional-areas/crtp/crtp_platform/
  crazyflie_driver::crtpPacket packet;
  packet.header = 220;  // Port 13 but byteshifted following https://github.com/bitcraze/crazyflie-lib-python/blob/master/cflib/crtp/crtpstack.py#L120-L132
  packet.size = 2;      // Payload length
  packet.data[0] = 1;   // Channel 0 -- Platform commands
  packet.data[1] = arm ? 1 : 0;  // Arm message (bool).

  // Call the sendPacket service to send the packet
  crazyflie_driver::sendPacket srv;
  srv.request.packet = packet;

  if (packet_client_.call(srv))
  {
    ROS_INFO("Arming request sent successfully, arm: %d", arm);
  }
  else
  {
    ROS_ERROR("Failed to send arming request");
  }
};

void SO3CmdToCrazyflie::reboot()
{
  ROS_INFO("Attempting to reboot Crazyflie...");
  
  // Create custom packet for rebooting. 
  crazyflie_driver::crtpPacket packet;
  packet.header = 255;  // 
  packet.size = 2;      // Payload length
  packet.data[0] = 0xFE;  
  packet.data[1] = 0x02;  // SYSOFF command. 

  // Call the sendPacket service to send the packet
  crazyflie_driver::sendPacket srv;
  srv.request.packet = packet;

  if (packet_client_.call(srv))
  {
    ROS_INFO("Powering down.");
  }
  else
  {
    ROS_ERROR("Failed to power down");
  }

  // Wait a little bit. 
  ros::Duration(0.5).sleep();  // Sleep for a short time.

  // Now send boot up command. 
  packet.data[1] = 0x03;        // SYSON command.
  srv.request.packet = packet;

  if (packet_client_.call(srv))
  {
    ROS_INFO("Powering up.");
  }
  else
  {
    ROS_ERROR("Failed to power up.");
  }

};

void SO3CmdToCrazyflie::odom_callback(const nav_msgs::Odometry::ConstPtr &odom)
{
  ros::Time current_time = odom->header.stamp;
  // Extract current angular velocity
  Eigen::Vector3d w_odom_current(odom->twist.twist.angular.x, 
    odom->twist.twist.angular.y, 
    odom->twist.twist.angular.z);

  if(!odom_set_)
  {
    // First time receiving odometry
    odom_set_ = true;
    w_odom_last = w_odom_current;
    last_odom_time_ = current_time;
    w_dot_odom_ = Eigen::Vector3d::Zero(); // Initialize with zero acceleration
  }
  else
  {
    // Calculate time delta
    double dt = (current_time - last_odom_time_).toSec();

    // Avoid division by zero or very small dt
    if(dt > 1e-6)
    {
      // Compute angular acceleration using finite differencing
      w_dot_odom_ = (w_odom_current - w_odom_last) / dt;

      // Update last values for next iteration
      w_odom_last = w_odom_current;
      last_odom_time_ = current_time;
    }
  }

  q_odom_ = Eigen::Quaterniond(odom->pose.pose.orientation.w, odom->pose.pose.orientation.x,
                               odom->pose.pose.orientation.y, odom->pose.pose.orientation.z);

  w_odom_ = w_odom_current;

  if(so3_cmd_set_ && ((ros::Time::now() - last_so3_cmd_time_).toSec() >= so3_cmd_timeout_))
  {
    // ROS_WARN("so3_cmd timeout. %f seconds since last command",
    //       (ros::Time::now() - last_so3_cmd_time_).toSec());
    const auto last_so3_cmd_ptr = boost::make_shared<kr_mav_msgs::SO3Command>(last_so3_cmd_);

    so3_cmd_callback(last_so3_cmd_ptr);
  }
}

void SO3CmdToCrazyflie::so3_cmd_callback(const kr_mav_msgs::SO3Command::ConstPtr &msg)
{
  if(!so3_cmd_set_)
    so3_cmd_set_ = true;

  // switch on motors
  if(msg->aux.enable_motors)
  {

    // First try to arm the drone.
    if(is_brushless_) // Only do so if CF is brushless (rosparam)
    { 
      if(!armed_)
      {
        for(int i=0; i<1; i++)
        {
        send_arming_request(true);
        }
        armed_ = true;
      };
    };

    // If the crazyflie motors are timed out, we need to send a zero message in order to get them to start
    if(motor_status_ < 3)
    {
      geometry_msgs::Twist::Ptr motors_vel_cmd(new geometry_msgs::Twist);
      crazy_cmd_vel_pub_.publish(motors_vel_cmd);
      last_so3_cmd_ = *msg;
      last_so3_cmd_time_ = msg->header.stamp;
      motor_status_ += 1;
      return;
    }
    // After sending zero message send min thrust
    if(motor_status_ < 10)
    {
      geometry_msgs::Twist::Ptr motors_vel_cmd(new geometry_msgs::Twist);
      motors_vel_cmd->linear.z = thrust_pwm_min_;
      crazy_cmd_vel_pub_.publish(motors_vel_cmd);
    }
    motor_status_ += 1;
  }
  else if(!msg->aux.enable_motors)
  {
    motor_status_ = 0;
    geometry_msgs::Twist::Ptr motors_vel_cmd(new geometry_msgs::Twist);
    crazy_cmd_vel_pub_.publish(motors_vel_cmd);
    last_so3_cmd_ = *msg;
    last_so3_cmd_time_ = msg->header.stamp;

    // Disarm motors
    if(is_brushless_)
    {
      if(armed_)
      {
        for(int i=0; i<1; i++)
        {
        send_arming_request(false);
        }
        armed_ = false;
        // Reboot Crazyflie to be armed again. 
        reboot();
      };
    };
    return;
  }

  /*
  // If the crazyflie motors are timed out, we need to send a zero message in order to get them to start
  if((ros::Time::now() - last_so3_cmd_time_).toSec() >= so3_cmd_timeout_){
    crazy_cmd_vel_pub_.publish(crazy_vel_cmd);
    // Keep in mind that this publishes two messages in a row, this may cause bandwith problems
  }
  */

  // grab desired forces and rotation from so3
  const Eigen::Vector3d f_des(msg->force.x, msg->force.y, msg->force.z);

  const Eigen::Quaterniond q_des(msg->orientation.w, msg->orientation.x, msg->orientation.y, msg->orientation.z);

  // check psi for stability
  const Eigen::Matrix3d R_des(q_des);
  const Eigen::Matrix3d R_cur(q_odom_);

  const float yaw_cur = std::atan2(R_cur(1, 0), R_cur(0, 0));
  const float yaw_des = std::atan2(R_des(1, 0), R_des(0, 0));

  // Map these into the current body frame (based on yaw)
  Eigen::Matrix3d R_des_new = R_des * Eigen::AngleAxisd(yaw_cur - yaw_des, Eigen::Vector3d::UnitZ());
  float pitch_des = -std::asin(R_des_new(2, 0));
  float roll_des = std::atan2(R_des_new(2, 1), R_des_new(2, 2));

  roll_des = roll_des * 180 / M_PI;
  pitch_des = pitch_des * 180 / M_PI;

  double thrust_f = f_des(0) * R_cur(0, 2) + f_des(1) * R_cur(1, 2) + f_des(2) * R_cur(2, 2);  // Force in Newtons
  // ROS_INFO("thrust_f is %2.5f newtons", thrust_f);
  thrust_f = std::max(thrust_f, 0.0);

  thrust_f = thrust_f * 1000 / 9.81;  // Force in grams
  // ROS_INFO("thrust_f is %2.5f grams", thrust_f);

  // ROS_INFO("coeffs are %2.2f, %2.2f, %2.2f", c1_, c2_, c3_);
  float thrust_pwm = c1_ + c2_ * std::sqrt(c3_ + thrust_f);

  // ROS_INFO("thrust_pwm is %2.5f from 0-1", thrust_pwm);

  thrust_pwm = thrust_pwm * thrust_pwm_max_;  // thrust_pwm mapped from 0-60000
  // ROS_INFO("thrust_pwm is calculated to be %2.5f in pwm", thrust_pwm);

  geometry_msgs::Twist::Ptr crazy_vel_cmd(new geometry_msgs::Twist);

  float e_yaw = yaw_des - yaw_cur;
  if(e_yaw > M_PI)
    e_yaw -= 2 * M_PI;
  else if(e_yaw < -M_PI)
    e_yaw += 2 * M_PI;

  float yaw_rate_des = ((-msg->kR[2] * e_yaw) - msg->angular_velocity.z) * (180 / M_PI);

  // TODO change this check to be a param
  // throttle the publish rate
  // if ((ros::Time::now() - last_so3_cmd_time_).toSec() > 1.0/30.0){
  if (send_ctbr_cmds_) {
    // float roll_rate_des = (msg->kOm[0] * (msg->angular_velocity.x - w_odom_[0])) * 180/M_PI;
    // float pitch_rate_des = (msg->kOm[1] * (msg->angular_velocity.y - w_odom_[1])) * 180/M_PI;
    // crazy_vel_cmd->linear.y = roll_rate_des;  // ctbr_cmd->angular_velocity.x = roll rate
    // crazy_vel_cmd->linear.x = pitch_rate_des;  // ctbr_cmd->angular_velocity.y = pitch rate

    // Get P terms from angular velocity error
    float roll_rate_p = msg->kOm[0] * (msg->angular_velocity.x - w_odom_[0]);
    float pitch_rate_p = msg->kOm[1] * (msg->angular_velocity.y - w_odom_[1]);
    
    // Get D terms from angular acceleration (optional kD gains could be added as parameters)
    float roll_rate_d = ang_acc_d_gain * w_dot_odom_[0];  // Adjust gain as needed
    float pitch_rate_d = ang_acc_d_gain * w_dot_odom_[1]; // Adjust gain as needed
    
    // Combine P and D terms to get desired body rates (in degrees/sec)
    float roll_rate_des = (roll_rate_p - roll_rate_d) * (180.0/M_PI);
    float pitch_rate_des = (pitch_rate_p - pitch_rate_d) * (180.0/M_PI);
    
    // Send the commands
    crazy_vel_cmd->linear.y = roll_rate_des;
    crazy_vel_cmd->linear.x = pitch_rate_des;

    // Working 
    // crazy_vel_cmd->linear.y = msg->angular_velocity.x;  // ctbr_cmd->angular_velocity.x = roll rate
    // crazy_vel_cmd->linear.x = msg->angular_velocity.y;  // ctbr_cmd->angular_velocity.y = pitch rate


  } else {
    crazy_vel_cmd->linear.y = roll_des + msg->aux.angle_corrections[0];
    crazy_vel_cmd->linear.x = pitch_des + msg->aux.angle_corrections[1];
  }
  crazy_vel_cmd->linear.z = CLAMP(thrust_pwm, thrust_pwm_min_, thrust_pwm_max_);

  // ROS_INFO("commanded thrust is %2.2f", CLAMP(thrust_pwm, thrust_pwm_min_, thrust_pwm_max_));

  crazy_vel_cmd->angular.z = yaw_rate_des;
  // ROS_INFO("commanded yaw rate is %2.2f", yaw_rate_des); //yaw_debug

  crazy_fast_cmd_vel_pub_.publish(crazy_vel_cmd);
  // save last so3_cmd
  last_so3_cmd_ = *msg;
  // last_so3_cmd_time_ = ros::Time::now();
  last_so3_cmd_time_ = msg->header.stamp;
  //}
  // else {
  //  ROS_INFO_STREAM("Commands too quick, time since is: " << (ros::Time::now() - last_so3_cmd_time_).toSec());
  //}
}

void SO3CmdToCrazyflie::onInit(void)
{
  ros::NodeHandle priv_nh(getPrivateNodeHandle());

  // get thrust scaling parameters
  // Note that this is ignoring a constant based on the number of props, which
  // is captured with the lin_cof_a variable later.
  //
  // TODO this is where we load thrust scaling stuff
  if(priv_nh.getParam("kp_yaw_rate", kp_yaw_rate_))
    ROS_INFO("kp yaw rate is %2.2f", kp_yaw_rate_);
  else
    ROS_FATAL("kp yaw rate not found");

  // get thrust scaling parameters
  if(priv_nh.getParam("c1", c1_) && priv_nh.getParam("c2", c2_) && priv_nh.getParam("c3", c3_))
    ROS_INFO("Using %2.2f, %2.2f, %2.2f for thrust mapping", c1_, c2_, c3_);
  else
    ROS_FATAL("Must set coefficients for thrust scaling");

  if(priv_nh.getParam("ang_acc_d_gain", ang_acc_d_gain))
    ROS_INFO("Using %2.2f for angular acceleration D gain", ang_acc_d_gain);
  else
    ROS_FATAL("Must set angular acceleration D gain");

  // get param for so3 command timeout duration
  priv_nh.param("so3_cmd_timeout", so3_cmd_timeout_, 0.1);

  // get param for whether or not the crazyflie is of type brushless.
  priv_nh.param("is_brushless", is_brushless_, false);

  // get param for sending ctbr cmds, default is to send TRPY commands as attitude
  priv_nh.param("send_ctbr_cmds", send_ctbr_cmds_, false);

  priv_nh.param("thrust_pwm_max", thrust_pwm_max_, 60000);
  priv_nh.param("thrust_pwm_min", thrust_pwm_min_, 10000);

  odom_set_ = false;
  so3_cmd_set_ = false;
  motor_status_ = 0;
  armed_ = false;
  arm_status_ = 0;

  // Retrieve mav_name parameter
  std::string mav_name;
  if (!priv_nh.getParam("mav_name", mav_name))
  {
    ROS_FATAL("mav_name parameter not found.");
    return;
  }

  // Construct the full service name
  std::string service_name = "/" + mav_name + "/send_packet";

  // Arming packet client. 
  packet_client_ = priv_nh.serviceClient<crazyflie_driver::sendPacket>(service_name);

  ROS_INFO("Waiting for send_packet service...");
  packet_client_.waitForExistence();
  ROS_INFO("send_packet service is available.");

  // TODO make sure this is publishing to the right place
  crazy_fast_cmd_vel_pub_ = priv_nh.advertise<geometry_msgs::Twist>("cmd_vel_fast", 10);

  crazy_cmd_vel_pub_ = priv_nh.advertise<geometry_msgs::Twist>("cmd_vel", 10);

  so3_cmd_sub_ =
      priv_nh.subscribe("so3_cmd", 1, &SO3CmdToCrazyflie::so3_cmd_callback, this, ros::TransportHints().tcpNoDelay());

  odom_sub_ =
      priv_nh.subscribe("odom", 10, &SO3CmdToCrazyflie::odom_callback, this, ros::TransportHints().tcpNoDelay());
}

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(SO3CmdToCrazyflie, nodelet::Nodelet);
