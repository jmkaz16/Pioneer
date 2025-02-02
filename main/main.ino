/*Motor controller using micro_ros serial set_microros_transports*/

#include <micro_ros_arduino.h>
#include <geometry_msgs/msg/twist.h>
#include <rcl/error_handling.h>
#include <rcl/rcl.h>
#include <rclc/executor.h>
#include <rclc/rclc.h>
#include <stdio.h>

#define EXECUTE_EVERY_N_MS(MS, X)          \
    do {                                   \
        static volatile int64_t init = -1; \
        if (init == -1) {                  \
            init = uxr_millis();           \
        }                                  \
        if (uxr_millis() - init > MS) {    \
            X;                             \
            init = uxr_millis();           \
        }                                  \
    } while (0)

bool micro_ros_init_successful;
enum states {
    WAITING_AGENT,
    AGENT_AVAILABLE,
    AGENT_CONNECTED,
    AGENT_DISCONNECTED
} state;

// ROS constants

#define RCCHECK(fn)                    \
    {                                  \
        rcl_ret_t temp_rc = fn;        \
        if ((temp_rc != RCL_RET_OK)) { \
            error_loop();              \
        }                              \
    }
#define RCSOFTCHECK(fn)                \
    {                                  \
        rcl_ret_t temp_rc = fn;        \
        if ((temp_rc != RCL_RET_OK)) { \
            error_loop();              \
        }                              \
    }

#define LED_PIN 2

#define M2INA 33
#define M2INB 32
#define M2PWM 25

#define M1INA 27
#define M1INB 26
#define M1PWM 13

// pin declaration
// Left wheel
int8_t L_encoderPin1 = 23;  // Encoder Output of pin1 must connected with intreput pin of Esp32.
int8_t L_encoderPin2 = 22;

// right wheel
int8_t R_encoderPin1 = 21;  // Encoder Output of pin1 must connected with intreput pin of Esp32.
int8_t R_encoderPin2 = 19;

// motor_ids
int8_t motor_id_L = 1;
int8_t motor_id_R = 2;

// parameters of the robot
float wheels_y_distance_ = 0.41;
float wheel_radius = 0.11;
float wheel_circumference_ = 2 * 3.14 * wheel_radius;
// int tickPerRevolution_LW = 612; //working
int tickPerRevolution_LW = 24895;  // working
int tickPerRevolution_RW = 24895;  // working

// for pid calculation
unsigned long startMillis = 0;
unsigned long currentMillis = 0;
unsigned long period = 50;  // period of position publishing in milliseconds

// for odometry publisher
unsigned long startMillis2 = 0;
unsigned long currentMillis2 = 0;
unsigned long period2 = 100;  // period of speed publishing in milliseconds

float setpoint_L;
float setpoint_R;

int u_max = 200;
int max_speed = 15;

// pid constants of left wheel
float kp_l = 17.0;    // 30
float ki_l = 22.00;   // 60
float kd_l = 0.3;  // 0.8
// pid constants of right wheel
float kp_r = 10.0;    // 30
float ki_r = 17.0;   // 25
float kd_r = 0.1;  // 0.05

// constant for speed filter
float ema_alpha = 0.7;

// motor driver parameters
volatile long L_encoder_count = 0;
volatile long R_encoder_count = 0;

// velocity of the motors
float speed_L_motor;
float speed_R_motor;

// motor declaration
const int motorFreq = 1000;     // PWM frequency
const int motorResolution = 8;  // PWM resolution (bits)
const int motorChannelA = 0;    // PWM channel for motor A
const int motorChannelB = 1;    // PWM channel for motor B

void motor_setup() {
    pinMode(M1INA, OUTPUT);
    pinMode(M1INB, OUTPUT);
    pinMode(M1PWM, OUTPUT);
    ledcSetup(motorChannelA, motorFreq, motorResolution);
    ledcAttachPin(M1PWM, motorChannelA);

    pinMode(M2INA, OUTPUT);
    pinMode(M2INB, OUTPUT);
    pinMode(M2PWM, OUTPUT);
    ledcSetup(motorChannelB, motorFreq, motorResolution);
    ledcAttachPin(M2PWM, motorChannelB);
}

float signal_L_motor;
float signal_R_motor;

// ROS2 DECLARATIONS
rcl_publisher_t speed_publisher;

rcl_subscription_t subscriber;

geometry_msgs__msg__Twist msg;
geometry_msgs__msg__Twist speed_msg;

rclc_executor_t executor;
rcl_allocator_t allocator;
rclc_support_t support;
rcl_node_t node;

void error_loop() {
    while (1) {
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        delay(100);
    }
}

// creating a class for motor control
class MotorController {
   public:
    int8_t motor_id;
    int8_t EncoderPinA;
    int8_t EncoderPinB;
    volatile long EncoderCount;
    volatile long CurrentPosition;
    volatile long PreviousPosition;
    volatile long CurrentTime;
    volatile long PreviousTime;
    volatile long CurrentTimeforError;
    volatile long PreviousTimeForError;
    float speed_filtered;
    float integral;
    float ederivative;
    float kp;
    float ki;
    float kd;
    float error;
    float previousError = 0;
    int tick;

    MotorController(int8_t Motor_id, int8_t EncoderA, int8_t EncoderB, int tickPerRevolution) {
        this->motor_id = Motor_id;
        this->EncoderPinA = EncoderA;
        this->EncoderPinB = EncoderB;
        this->tick = tickPerRevolution;
        pinMode(EncoderPinA, INPUT);
        pinMode(EncoderPinB, INPUT);
    }

    // initializing the parameters of PID controller
    void initPID(float proportionalGain, float integralGain, float derivativeGain) {
        kp = proportionalGain;
        ki = integralGain;
        kd = derivativeGain;
    }

    // function return rpm of the motor using the encoder tick values
    float getSpeed() {
        CurrentPosition = EncoderCount;
        CurrentTime = millis();
        float delta1 = ((float)CurrentTime - PreviousTime) / 1.0e3;
        float velocity = ((float)CurrentPosition - PreviousPosition) / delta1;
        float speed_unfiltered = (velocity / tick) * (2 * 3.141592);
        speed_filtered = (ema_alpha)*speed_unfiltered + (1 - ema_alpha) * speed_filtered;
        PreviousPosition = CurrentPosition;
        PreviousTime = CurrentTime;

        if (abs(speed_filtered) > max_speed) {
            speed_filtered = 0;
        }
        return speed_filtered;
    }

    // pid controller
    float pid(float setpoint, float feedback) {
        CurrentTimeforError = millis();
        float delta2 = ((float)CurrentTimeforError - PreviousTimeForError) / 1.0e3;
        error = setpoint - feedback;
        integral += (ki * error * delta2);
        ederivative = (error - previousError) / delta2;
        float control_signal = (kp * error) + integral + (kd * ederivative);

        // codigo antiwindup
        if (control_signal > u_max) {
            integral -= control_signal - u_max;
            control_signal = u_max;
        } else if (control_signal < -u_max) {
            integral += -u_max - control_signal;
            control_signal = -u_max;
        }

        previousError = error;
        PreviousTimeForError = CurrentTimeforError;

        return control_signal;
    }

    void stop() {
        ledcWrite(motorChannelA, 0);
        ledcWrite(motorChannelB, 0);
        integral = 0;
        // previousError=0;
    }
};

    // move the robot wheels based the control signal generated by the pid controller
    void moveBase(float signalL, float signalR) {
        if(signalL > 0){
            digitalWrite(M1INA, HIGH);
            digitalWrite(M1INB, LOW);
        }else if(signalL < 0){
            digitalWrite(M1INA, LOW);
            digitalWrite(M1INB, HIGH);
        }

        if(signalR > 0){
            digitalWrite(M2INA, HIGH);
            digitalWrite(M2INB, LOW);
        }else if(signalR < 0){
            digitalWrite(M2INA, LOW);
            digitalWrite(M2INB, HIGH);
        }

        int pwmL = constrain(abs(signalL), 0, 255);
        int pwmR = constrain(abs(signalR), 0, 255);

        ledcWrite(motorChannelA, pwmL);
        ledcWrite(motorChannelB, pwmR);
    }

// creating objects for right wheel and left wheel
MotorController leftWheel(motor_id_L, L_encoderPin1, L_encoderPin2, tickPerRevolution_LW);
MotorController rightWheel(motor_id_R, R_encoderPin1, R_encoderPin2, tickPerRevolution_RW);

// interrupt function for left wheel encoder.
void IRAM_ATTR updateEncoderL() {
    if (digitalRead(leftWheel.EncoderPinA) > digitalRead(leftWheel.EncoderPinB))
        leftWheel.EncoderCount++;
    else
        leftWheel.EncoderCount--;
}

// interrupt function for right wheel encoder
void IRAM_ATTR updateEncoderR() {
    if (digitalRead(rightWheel.EncoderPinA) > digitalRead(rightWheel.EncoderPinB))
        rightWheel.EncoderCount--;
    else
        rightWheel.EncoderCount++;
}

void diff_model(float linear_x, float angular_z) {
    setpoint_L = (linear_x - angular_z * wheels_y_distance_ / 2) / wheel_radius;
    setpoint_R = (linear_x + angular_z * wheels_y_distance_ / 2) / wheel_radius;
}

bool create_entities() {
    allocator = rcl_get_default_allocator();

    rclc_support_init(&support, 0, NULL, &allocator);
    rclc_node_init_default(&node, "micro_ros_diff_drive", "", &support);

    // create subscriber cmd_vel
    RCCHECK(rclc_subscription_init_best_effort(
        &subscriber,
        &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, Twist),
        "cmd_vel"));

    // create publisher speed
    RCCHECK(rclc_publisher_init_best_effort(
        &speed_publisher,
        &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, Twist),
        "debug/speed"));

    // create executor
    executor = rclc_executor_get_zero_initialized_executor();
    rclc_executor_init(&executor, &support.context, 1, &allocator);
    rclc_executor_add_subscription(&executor, &subscriber, &msg, &speed_callback, ON_NEW_DATA);

    return true;
}

void destroy_entities() {
    rmw_context_t *rmw_context = rcl_context_get_rmw_context(&support.context);
    (void)rmw_uros_set_context_entity_destroy_session_timeout(rmw_context, 0);

    rcl_publisher_fini(&speed_publisher, &node);
    rcl_subscription_fini(&subscriber, &node);
    rclc_executor_fini(&executor);
    rcl_node_fini(&node);
    rclc_support_fini(&support);
}

void ros_speedpub(float speed_L, float speed_R) {
    speed_msg.linear.x = speed_L;
    speed_msg.linear.y = speed_R;
    speed_msg.linear.z = setpoint_L;
    speed_msg.angular.x = setpoint_R;
    speed_msg.angular.y = signal_L_motor;
    speed_msg.angular.z = signal_R_motor;

    rcl_publish(&speed_publisher, &speed_msg, NULL);
}

void speed_callback(const void *msgin) {
    const geometry_msgs__msg__Twist *msg = (const geometry_msgs__msg__Twist *)msgin;
    diff_model(msg->linear.x, msg->angular.z);
}

void setup() {
    // inicializacion de motor_controller
    leftWheel.initPID(kp_l, ki_l, kd_l);
    rightWheel.initPID(kp_r, ki_r, kd_r);

    // set up de hardware
    motor_setup();
    // interrupciones para encoders
    attachInterrupt(digitalPinToInterrupt(leftWheel.EncoderPinA), updateEncoderL, RISING);
    attachInterrupt(digitalPinToInterrupt(rightWheel.EncoderPinA), updateEncoderR, RISING);

    // setup de ros
    set_microros_transports();

    state = WAITING_AGENT;

    // allocator = rcl_get_default_allocator();

    // create init_options
    // rclc_support_init(&support, 0, NULL, &allocator);

    // create node
    // rclc_node_init_default(&node, "micro_ros_diff_drive", "", &support);

    // create subscriber cmd_vel
    // RCCHECK(rclc_subscription_init_best_effort(
    //   &subscriber,
    //   &node,
    //   ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, Twist),
    //   "cmd_vel"));

    // create publisher speed
    // RCCHECK(rclc_publisher_init_best_effort(
    //   &speed_publisher,
    //   &node,
    //   ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, Twist),
    //   "debug/speed"));

    // create publisher odom
    // RCCHECK(rclc_publisher_init_default(
    //   &odom_publisher,
    //   &node,
    //   ROSIDL_GET_MSG_TYPE_SUPPORT(nav_msgs, msg, Odometry),
    //   "odom/unfiltered"));

    // create executor
    // rclc_executor_init(&executor, &support.context, 1, &allocator);
    // rclc_executor_add_subscription(&executor, &subscriber, &msg, &speed_callback, ON_NEW_DATA);
}

void loop() {
    switch (state) {
        case WAITING_AGENT:
            EXECUTE_EVERY_N_MS(500, state = (RMW_RET_OK == rmw_uros_ping_agent(100, 1)) ? AGENT_AVAILABLE : WAITING_AGENT;);
            break;
        case AGENT_AVAILABLE:
            state = (true == create_entities()) ? AGENT_CONNECTED : WAITING_AGENT;
            if (state == WAITING_AGENT) {
                destroy_entities();
            };
            break;
        case AGENT_CONNECTED:
            EXECUTE_EVERY_N_MS(200, state = (RMW_RET_OK == rmw_uros_ping_agent(100, 1)) ? AGENT_CONNECTED : AGENT_DISCONNECTED;);
            if (state == AGENT_CONNECTED) {
                RCSOFTCHECK(rclc_executor_spin_some(&executor, RCL_MS_TO_NS(50)));
            }
            break;
        case AGENT_DISCONNECTED:
            destroy_entities();
            state = WAITING_AGENT;
            break;
        default:
            break;
    }

    delay(0);

    if (state == AGENT_CONNECTED) {
        currentMillis = millis();

        if (currentMillis - startMillis >= period) {
            speed_L_motor = leftWheel.getSpeed();
            speed_R_motor = rightWheel.getSpeed();

            signal_L_motor = leftWheel.pid(setpoint_L, speed_L_motor);
            signal_R_motor = rightWheel.pid(setpoint_R, speed_R_motor);

            if (setpoint_L == 0 && setpoint_R == 0) {
                leftWheel.stop();
                rightWheel.stop();
            } else {
                moveBase(signal_L_motor, signal_R_motor);
   
            }

            startMillis = currentMillis;
        }

        // Funcion para publicar la velocidad
        currentMillis2 = millis();
        if (currentMillis2 - startMillis2 >= period2) {
            ros_speedpub(speed_L_motor, speed_R_motor);
            startMillis2 = currentMillis2;
        }
    } else {
        leftWheel.stop();
        rightWheel.stop();
    }
}
     
