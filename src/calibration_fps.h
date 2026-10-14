// 60 FPS on the lag calibration screens (see calibration_fps.cpp).

#pragma once

namespace rb3pc {

// Called once per game frame before it presents; sleeps to hold 60 FPS
// while a calibration screen is up, and returns at once otherwise.
void PaceCalibrationFrame();

}  // namespace rb3pc
