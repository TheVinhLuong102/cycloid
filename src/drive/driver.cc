#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <Eigen/Dense>

#include "drive/config.h"
#include "drive/controller.h"
#include "drive/driver.h"
#include "drive/flushthread.h"
#include "hw/cam/cam.h"
#include "hw/car/car.h"
#include "hw/imu/imu.h"
#include "hw/input/js.h"
#include "localization/ceiltrack/ceiltrack.h"
#include "ui/display.h"

// hardcoded garbage for the time being
const float CEILHOME_X = -3.03, CEILHOME_Y = 0.73, CEILHOME_THETA = 0;
const float CEIL_HEIGHT = 8.25*0.3048;
const float CEIL_X_GRID = 0.3048*10/CEIL_HEIGHT;
const float CEIL_Y_GRID = 0.3048*12/CEIL_HEIGHT;

// const int PWMCHAN_STEERING = 14;
// const int PWMCHAN_ESC = 15;

struct CarState {
  Eigen::Vector3f accel, gyro;
  int8_t throttle, steering;
  float wheel_dist, wheel_v;
  float ceiltrack_pos[3];

  CarState() : accel(0, 0, 0), gyro(0, 0, 0) {
    throttle = 0;
    steering = 0;
    wheel_dist = 0;
    wheel_v = 0;
    SetHome();
  }

  void SetHome() {
    ceiltrack_pos[0] = CEILHOME_X;
    ceiltrack_pos[1] = CEILHOME_Y;
    ceiltrack_pos[2] = CEILHOME_THETA;
  }

  // 2 3-float vectors, 3 uint8s, 2 4-uint16 arrays
  int SerializedSize() { return 8 + 4 * 3 * 2 + 2 + 2 * 4; }

  int Serialize(uint8_t *buf, int bufsiz) {
    int len = SerializedSize();
    assert(bufsiz >= len);
    memcpy(buf, "CSt1", 4);
    memcpy(buf + 4, &len, 4);
    buf += 8;
    memcpy(buf + 0, &throttle, 1);
    memcpy(buf + 1, &steering, 1);
    memcpy(buf + 2, &accel[0], 4);
    memcpy(buf + 2 + 4, &accel[1], 4);
    memcpy(buf + 2 + 8, &accel[2], 4);
    memcpy(buf + 14, &gyro[0], 4);
    memcpy(buf + 14 + 4, &gyro[1], 4);
    memcpy(buf + 14 + 8, &gyro[2], 4);
    memcpy(buf + 26, &wheel_dist, 4);
    memcpy(buf + 30, &wheel_v, 4);
    return len;
  }
} carstate_;

Driver::Driver(const INIReader &ini, CeilingTracker *ceil, ObstacleDetector *od,
               FlushThread *ft, IMU *imu, JoystickInput *js, UIDisplay *disp)
    : ceiltrack_(ceil),
      obstacledetect_(od),
      flush_thread_(ft),
      imu_(imu),
      js_(js),
      display_(disp),
      gyro_bias_(0, 0, 0) {
  output_fd_ = -1;
  frame_ = 0;
  frameskip_ = 0;
  autodrive_ = false;
  memset(&last_t_, 0, sizeof(last_t_));
  if (config_.Load()) {
    fprintf(stderr, "Loaded driver configuration\n");
  }
  js_throttle_ = 0;
  js_steering_ = 0;

  config_item_ = 0;
  x_down_ = y_down_ = false;
  done_ = false;
}

bool Driver::StartRecording(const char *fname, int frameskip) {
  frameskip_ = frameskip;
  frame_ = 0;
  if (!strcmp(fname, "-")) {
    output_fd_ = fileno(stdout);
  } else {
    output_fd_ = open(fname, O_CREAT | O_TRUNC | O_WRONLY, 0666);
  }
  if (output_fd_ == -1) {
    perror(fname);
    return false;
  }
  printf("--- recording %s ---\n", fname);
  // write header IFF chunk immediately: store the car config
  int siz = config_.SerializedSize();
  uint8_t *hdrbuf = new uint8_t[siz];
  config_.Serialize(hdrbuf, siz);
  write(output_fd_, hdrbuf, siz);
  delete[] hdrbuf;
  return true;
}

bool Driver::IsRecording() { return output_fd_ != -1; }

void Driver::StopRecording() {
  if (output_fd_ == -1) {
    return;
  }
  flush_thread_->AddEntry(output_fd_, NULL, -1);
  output_fd_ = -1;
}

Driver::~Driver() { StopRecording(); }

// recording data is in IFF format, can be read with python chunk interface:
// ck = chunk.Chunk(file, align=False, bigendian=False, inclheader=True)
// each frame is stored in a CYCF chunk which includes an 8-byte timestamp,
// and further set of chunks encoded by each piece below.
void Driver::QueueRecordingData(const timeval &t, uint8_t *buf, size_t length) {
  uint32_t chunklen = 8 + 8;           // iff header, timestamp
  uint32_t yuvcklen = length + 8 + 2;  // iff header, width, camera frame
  // each of the following entries is expected to be a valid
  // IFF chunk on its own
  chunklen += carstate_.SerializedSize();
  chunklen += controller_.SerializedSize();
  chunklen += yuvcklen;

  // copy our frame, push it onto a stack to be flushed
  // asynchronously to sdcard
  uint8_t *chunkbuf = new uint8_t[chunklen];
  // write length + timestamp header
  memcpy(chunkbuf, "CYCF", 4);
  memcpy(chunkbuf + 4, &chunklen, 4);
  memcpy(chunkbuf + 8, &t.tv_sec, 4);
  memcpy(chunkbuf + 12, &t.tv_usec, 4);
  int ptr = 16;
  ptr += carstate_.Serialize(chunkbuf + ptr, chunklen - ptr);
  ptr += controller_.Serialize(chunkbuf + ptr, chunklen - ptr);

  // write the 640x480 yuv420 buffer last
  memcpy(chunkbuf + ptr, "Y420", 4);
  memcpy(chunkbuf + ptr + 4, &yuvcklen, 4);
  uint16_t framewidth = 640;  // hardcoded, fixme
  memcpy(chunkbuf + ptr + 8, &framewidth, 2);
  memcpy(chunkbuf + ptr + 10, buf, length);

  flush_thread_->AddEntry(output_fd_, chunkbuf, chunklen);
}

  // Update controller from gyro and wheel encoder inputs

  // Update controller and UI from camera
void Driver::UpdateFromCamera(uint8_t *buf, float dt) {
  ceiltrack_->Update(buf, 240, CEIL_X_GRID, CEIL_Y_GRID,
                     carstate_.ceiltrack_pos, 2, false);
  float xytheta[3];
  // convert ceiling homogeneous coordinates to actual meters on the ground
  // also we need to convert from bottom-up to top-down coordinates so we negate
  // through
  xytheta[0] = -carstate_.ceiltrack_pos[0] * CEIL_HEIGHT;
  xytheta[1] = -carstate_.ceiltrack_pos[1] * CEIL_HEIGHT;
  xytheta[2] = -carstate_.ceiltrack_pos[2];

  obstacledetect_->Update(buf, 40, 150);  // FIXME(a1k0n): needs config
  const int32_t *pcar = obstacledetect_->GetCarPenalties();
  const int32_t *pcone = obstacledetect_->GetConePenalties();

  controller_.UpdateLocation(config_, xytheta);
  controller_.Plan(config_, pcar, pcone);

  // display_.UpdateConeView(buf, 0, NULL);
  // display_->UpdateEncoders(carstate_.wheel_pos);
  // FIXME: hardcoded map size 20mx10m
  if (display_) {
    display_->UpdateCeiltrackView(xytheta, CEIL_X_GRID * CEIL_HEIGHT,
                                  CEIL_Y_GRID * CEIL_HEIGHT, 20, 10);
  }
}

  // Called each camera frame, 30Hz
void Driver::OnCameraFrame(uint8_t *buf, size_t length) {
  struct timeval t;
  gettimeofday(&t, NULL);
  frame_++;

  float dt = t.tv_sec - last_t_.tv_sec + (t.tv_usec - last_t_.tv_usec) * 1e-6;
  if (dt > 0.1 && last_t_.tv_sec != 0) {
    fprintf(stderr,
            "CameraThread::OnFrame: WARNING: "
            "%fs gap between frames?!\n",
            dt);
  }

  UpdateFromCamera(buf, dt);

  if (IsRecording() && frame_ > frameskip_) {
    frame_ = 0;
    QueueRecordingData(t, buf, length);
  }

  last_t_ = t;
}

// Called each control loop frame, 100Hz
// N.B. this can be called concurrently with OnFrame in a separate thread
bool Driver::OnControlFrame(CarHW *car, float dt) {
  if (js_) {
    js_->ReadInput(this);
  }

  Eigen::Vector3f gyro;
  imu_->ReadIMU(&carstate_.accel, &gyro);
  gyro_last_ = 0.95 * gyro_last_ + 0.05 * gyro;
  carstate_.gyro = gyro - gyro_bias_;

  // a = v^2 k = v w
  // v = a/w
  float ds, v;
  if (car->GetWheelMotion(&ds, &v)) {  // use wheel encoders if we have 'em
    carstate_.wheel_v = v;
  } else {
    // otherwise try to use the acceleromters/gyros to guess
    // FIXME(a1k0n): do these axes need configuration in the .ini?
    carstate_.wheel_v = 0.95 * (carstate_.wheel_v - 9.8*carstate_.accel[1]*dt);
    if (fabsf(carstate_.gyro[2] > 0.1)) {
      carstate_.wheel_v += 0.05 * fabsf(carstate_.accel[0] / carstate_.gyro[2]);
    }
  }
  controller_.UpdateState(config_, carstate_.accel, carstate_.gyro,
                          carstate_.wheel_v, dt);

  float u_a = carstate_.throttle / 127.0;
  float u_s = carstate_.steering / 127.0;
  if (controller_.GetControl(config_, js_throttle_ / 32767.0,
                             js_steering_ / 32767.0, &u_a, &u_s, dt, autodrive_,
                             frame_)) {
    uint8_t leds = (frame_ & 4);    // blink green LED
    leds |= IsRecording() ? 2 : 0;  // solid red when recording
    car->SetControls(leds, u_a, u_s);
  }
  carstate_.throttle = 127*u_a;
  carstate_.steering = 127*u_s;

  return !done_;
}

void Driver::OnDPadPress(char direction) {
  int16_t *value = ((int16_t *)&config_) + config_item_;
  switch (direction) {
    case 'U':
      --config_item_;
      if (config_item_ < 0) config_item_ = DriverConfig::N_CONFIGITEMS - 1;
      fprintf(stderr, "\n");
      break;
    case 'D':
      ++config_item_;
      if (config_item_ >= DriverConfig::N_CONFIGITEMS) config_item_ = 0;
      fprintf(stderr, "\n");
      break;
    case 'L':
      if (y_down_) {
        *value -= 100;
      } else if (x_down_) {
        *value -= 10;
      } else {
        --*value;
      }
      break;
    case 'R':
      if (y_down_) {
        *value += 100;
      } else if (x_down_) {
        *value += 10;
      } else {
        ++*value;
      }
      break;
  }
  UpdateDisplay();
}

void Driver::OnButtonPress(char button) {
  struct timeval tv;
  gettimeofday(&tv, NULL);

  switch (button) {
    case '+':  // start button: start recording
      if (!IsRecording()) {
        char fnamebuf[256];
        time_t start_time = time(NULL);
        struct tm start_time_tm;
        localtime_r(&start_time, &start_time_tm);
        strftime(fnamebuf, sizeof(fnamebuf), "cycloid-%Y%m%d-%H%M%S.rec",
                 &start_time_tm);
        if (StartRecording(fnamebuf, 0)) {
          fprintf(stderr, "%ld.%06ld started recording %s\n", tv.tv_sec,
                  tv.tv_usec, fnamebuf);
          if (display_) display_->UpdateStatus(fnamebuf, 0xffe0);
        }
      }
      break;
    case '-':  // select button: stop recording
      if (IsRecording()) {
        StopRecording();
        fprintf(stderr, "%ld.%06ld stopped recording\n", tv.tv_sec, tv.tv_usec);
        if (display_) display_->UpdateStatus("recording stopped", 0xffff);
      }
      break;
    case 'H':  // home button: init to start line
      carstate_.SetHome();
      gyro_bias_ = gyro_last_;
      printf("gyro bias %0.3f %0.3f %0.3f\n", gyro_bias_[0], gyro_bias_[1],
             gyro_bias_[2]);
      if (display_) display_->UpdateStatus("starting line", 0x07e0);
      break;
    case 'L':
      if (!autodrive_) {
        fprintf(stderr, "%ld.%06ld autodrive ON\n", tv.tv_sec, tv.tv_usec);
        autodrive_ = true;
      }
      break;
    case 'B':
      controller_.ResetState();
      if (config_.Load()) {
        fprintf(stderr, "config loaded\n");
        int16_t *values = ((int16_t *)&config_);
        if (display_) {
          display_->UpdateConfig(DriverConfig::confignames,
                                 DriverConfig::N_CONFIGITEMS, config_item_,
                                 values);
          display_->UpdateStatus("config loaded", 0xffff);
        }
      }
      fprintf(stderr, "reset kalman filter\n");
      break;
    case 'A':
      if (config_.Save()) {
        fprintf(stderr, "config saved\n");
        if (display_) display_->UpdateStatus("config saved", 0xffff);
      }
      break;
    case 'X':
      x_down_ = true;
      break;
    case 'Y':
      y_down_ = true;
      break;
  }
}

void Driver::OnButtonRelease(char button) {
  struct timeval tv;
  gettimeofday(&tv, NULL);

  switch (button) {
    case 'L':
      if (autodrive_) {
        autodrive_ = false;
        fprintf(stderr, "%ld.%06ld autodrive OFF\n", tv.tv_sec, tv.tv_usec);
      }
      break;
    case 'X':
      x_down_ = false;
      break;
    case 'Y':
      y_down_ = false;
      break;
  }
}

void Driver::OnAxisMove(int axis, int16_t value) {
  switch (axis) {
    case 1:  // left stick y axis
      js_throttle_ = -value;
      break;
    case 2:  // right stick x axis
      js_steering_ = value;
      break;
  }
}

void Driver::UpdateDisplay() {
  // hack because all config values are int16_t's in 1/100th steps
  int16_t *values = ((int16_t *)&config_);
  int16_t value = values[config_item_];
  // FIXME: does this work for negative values?
  fprintf(stderr, "%s %d.%02d\r", DriverConfig::confignames[config_item_], value / 100,
          value % 100);

  if (display_)
    display_->UpdateConfig(DriverConfig::confignames,
                           DriverConfig::N_CONFIGITEMS, config_item_, values);
}

