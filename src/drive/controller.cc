#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "drive/controller.h"

using Eigen::Vector3f;

const float V_ALPHA = 0.3;

// servo closed loop response bandwidth (measured)
const float BW_SRV = 2*M_PI*4;  // Hz

const float M_K1 = 2.58;  // DC motor response constants (measured)
const float M_K2 = 0.093;
const float M_K3 = 0.218;
const float M_OFFSET = 0.103;  // minimum control input (dead zone)

const float GEOM_LF = 6.5*.0254;  // car geometry; A = CG to front axle length
const float GEOM_LR = 5*.0254;  // CG to rear axle (m)

DriveController::DriveController() {
  ResetState();
  if (!track_.LoadTrack("track.txt")) {
    fprintf(stderr, "***WARNING: NO TRACK LOADED; check track.txt***\n");
  }
}

void DriveController::ResetState() {
  vr_ = vf_ = 0;
  w_ = 0;
  ierr_v_ = 0;
  ierr_w_ = 0;
}

static inline float clip(float x, float min, float max) {
  if (x < min) return min;
  if (x > max) return max;
  return x;
}

void DriveController::UpdateState(const DriverConfig &config,
    const Vector3f &accel, const Vector3f &gyro,
    uint8_t servo_pos, const uint16_t *wheel_delta, float dt) {

  // FIXME: hardcoded servo calibraiton
  delta_ = (servo_pos - 126.5) / 121.3;

  // update front/rear velocity estimate through crude filter
  vf_ *= (1 - V_ALPHA);
  vf_ += V_ALPHA * V_SCALE * 0.5*(wheel_delta[0] + wheel_delta[1])/dt;
  vr_ *= (1 - V_ALPHA);
  vr_ += V_ALPHA * V_SCALE * 0.5*(wheel_delta[2] + wheel_delta[3])/dt;

  w_ = gyro[2];
}

// this is the main autodrive control system
float DriveController::TargetCurvature(const DriverConfig &config) {
  float cx, cy, nx, ny, k;
  if (!track_.GetTarget(x_, y_, &cx, &cy, &nx, &ny, &k)) {
    return 2;  // circle right if you're confused
  }

  // (nx, ny) is the vector pointing towards +y (left)
  float ye = ((x_ - cx)*nx + (y_ - cy)*ny);

  float C = cos(theta_), S = sin(theta_);

  // the car's "y" coordinate is (-S, C); measure cos/sin psi
  float Cp = -S*nx + C*ny;
  float Sp = S*ny + C*nx;
  float Cpy = Cp / (1 - k * ye);

  float Kpy = config.steering_kpy * 0.01;
  float Kvy = config.steering_kvy * 0.01;
  float targetk = Cpy*(ye*Cpy*(-Kpy*Cp) + Sp*(k*Sp - Kvy*Cp) + k);

  // update control state for datalogging
  ye_ = ye;
  psie_ = atan2(Sp, Cp);
  k_ = k;
  target_k_ = targetk;

  return targetk;
}

bool DriveController::GetControl(const DriverConfig &config,
    float throttle_in, float steering_in,
    float *throttle_out, float *steering_out, float dt,
    bool autodrive, int frameno) {

  // okay, let's control for yaw rate!
  // throttle_in controls vmax (w.r.t. the configured value)
  // steering_in controls desired curvature

  // compute target curvature at all times, just for datalogging purposes
  float autok = TargetCurvature(config);

  // if we're braking or coasting, just control that manually
  if (!autodrive && throttle_in <= 0) {
    *throttle_out = throttle_in;
    *steering_out = -steering_in;  // yaw is backwards
    ierr_w_ = 0;  // also reset integrators
    ierr_v_ = 0;
    return true;
  }

  // max curvature is 1m radius
  // use a quadratic curve to give finer control near center
  float k = -steering_in * 2 * fabs(steering_in);
  float vmax = throttle_in * config.speed_limit * 0.01;
  if (autodrive) {
    k = autok;
    vmax = config.speed_limit * 0.01;
  }

  float kmin = config.traction_limit * 0.01 / (vmax*vmax);

  float target_v = vmax;
  if (fabs(k) > kmin) {  // any curvature more than this will reduce speed
    target_v = sqrt(config.traction_limit * 0.01 / fabs(k));

    // maintain an optimal slip ratio with 0 lateral velocity
    // by adjusting speed until vf = vr*cos(delta) - w*Lf*sin(delta)
    // vr = (vf + w*Lf*sin(delta)) / cos(delta)
    float vr_slip_target = (vf_ + w_*GEOM_LF*sin(delta_)) / cos(delta_);
    if (vr_slip_target < target_v && vr_slip_target > 1.0) {
      printf("[%d] using slip target %f (vf=%f vr=%f)\n",
          frameno, vr_slip_target, vf_, vr_);
      target_v = vr_slip_target;
    }
  }

  // use current velocity to determine target yaw rate
  // this yaw rate should be achievable with our tires given the slip rate
  // limit above
  float target_w = k*vr_;

  float err_v = vr_ - target_v;
  float err_w = w_ - target_w;

  float BW_w = 2*M_PI*0.01*config.yaw_bw;

  // *steering_out = clip(-BW_w/target_v * (ierr_w_ + err_w / BW_SRV), -1, 1);
  // why did i divide by target_v? that seems crazy and in practice it goes nuts
  // at low speeds unless BW_w is tiny.
  *steering_out = clip(-BW_w * (ierr_w_ + err_w / BW_SRV), -1, 1);

  float BW_v = 2*M_PI*0.01*config.motor_bw;
  float Kp = BW_v / (M_K1 - M_K2*vr_);
  float Ki = M_K3;
  *throttle_out = clip(-Kp*(err_v + Ki*ierr_v_) + M_OFFSET, 0, 1);
  if (*throttle_out == 0 && vr_ > 0) {  // handle braking
    // alternate control law
    float Kp2 = BW_v / (-M_K2*vr_);
    *throttle_out = clip(Kp2*(err_v + Ki*ierr_v_ - M_OFFSET), -1, 0);
  }

  // don't wind-up at control limits
  if (*throttle_out > -1 && *throttle_out < 1) {
    ierr_v_ += dt*err_v;
  }

  if ((*steering_out > -1 && *steering_out < 1) ||
      (err_w > 0 && ierr_w_ < 0) || (err_w < 0 && ierr_w_ > 0)) {
    // don't windup integrator if we're maxed out
    ierr_w_ += dt*err_w;
  }

  // update state for datalogging
  target_v_ = target_v;
  target_w_ = target_w;
  bw_w_ = BW_w;
  bw_v_ = BW_v;

  return true;
}

int DriveController::SerializedSize() const {
  return sizeof(float)*17;
}

int DriveController::Serialize(uint8_t *buf, int buflen) const {
  assert(buflen >= 68);

  memcpy(buf, &x_, 4);
  memcpy(buf+4, &y_, 4);
  memcpy(buf+8, &theta_, 4);
  memcpy(buf+12, &vf_, 4);
  memcpy(buf+16, &vr_, 4);
  memcpy(buf+20, &w_, 4);
  memcpy(buf+24, &ierr_v_, 4);
  memcpy(buf+28, &ierr_w_, 4);
  memcpy(buf+32, &delta_, 4);

  memcpy(buf+36, &target_k_, 4);
  memcpy(buf+40, &target_v_, 4);
  memcpy(buf+44, &target_w_, 4);
  memcpy(buf+48, &ye_, 4);
  memcpy(buf+52, &psie_, 4);
  memcpy(buf+56, &k_, 4);
  memcpy(buf+60, &bw_w_, 4);
  memcpy(buf+64, &bw_v_, 4);

  return 68;
}

void DriveController::Dump() const {
  printf("targetkvw %f %f %f v %f k %f windup %f %f",
      target_k_, target_v_, target_w_, vr_, k_, ierr_v_, ierr_w_);
}
