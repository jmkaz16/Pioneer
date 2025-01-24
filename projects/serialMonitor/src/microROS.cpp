#include "microROS.h"

extern float vel;

microRos::microRos(){
}

void microRos::initialize(){
    //Serial.begin(115200);
    set_microros_serial_transports(Serial);

    allocator = rcl_get_default_allocator();
    rclc_support_init(&support, 0, NULL, &allocator);
    rclc_node_init_default(&node, "micro_ros_platformio_node", "", &support);

    // create subscriber
    rclc_subscription_init_best_effort(
        &subscriber,
        &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, Twist),
        "cmd_vel");
    
    // create publisher
    rclc_publisher_init_best_effort( 
        &publisher,
        &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, String),
        "serial_monitor");

    //create executor
    rclc_executor_init(&executor, &support.context, 1, &allocator);
    rclc_executor_add_subscription(&executor, &subscriber, &cmd_vel_msg, &microRos::cmd_velCallback, ON_NEW_DATA);
}

void microRos::publish(){
    serial_monitor_msg.data.data = (char *)"Hello World!";
    serial_monitor_msg.data.size = strlen(serial_monitor_msg.data.data);
    serial_monitor_msg.data.capacity = serial_monitor_msg.data.size + 1;
    rcl_ret_t ret = rcl_publish(&publisher, &serial_monitor_msg, NULL);
    if (ret != RCL_RET_OK) {
      std::cout<<"Error al publicar el mensaje"<<std::endl;
    }
}

// suscriber callback
void microRos::cmd_velCallback(const void * msgin)
{  
  const geometry_msgs__msg__Twist * msg = (const geometry_msgs__msg__Twist *)msgin;
  vel= msg->linear.x;
}

void microRos::start(){
  //delay(1);
  rclc_executor_spin_some(&executor, RCL_MS_TO_NS(100));
}