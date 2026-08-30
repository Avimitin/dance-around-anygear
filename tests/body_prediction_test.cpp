#include "vp4u/body_prediction.h"

#include <cmath>
#include <iostream>

namespace {

bool near(float value, float expected, float tolerance = 1.0e-5f) {
    return std::fabs(value - expected) <= tolerance;
}

void tracked(vp4u::ParsedBody* body, int joint, float x) {
    body->TrackingState = 2;
    body->TrackingId = 1;
    body->JointData[joint].TrackingState = 2;
    body->JointData[joint].PositionX = x;
}

} // namespace

int main() {
    vp4u::ParsedBody previous{};
    vp4u::ParsedBody latest{};
    tracked(&previous, vp4u::JT_HandLeft, 0.0f);
    tracked(&latest, vp4u::JT_HandLeft, 0.05f);

    vp4u::ParsedBody output{};
    vp4u::predict_body(
        &output, previous, latest,
        1000.0, 1050.0, 1080.0, 20.0f);
    if (!near(output.JointData[vp4u::JT_HandLeft].PositionX, 0.10f)) {
        std::cerr << "capture age and configured prediction were not applied\n";
        return 1;
    }

    vp4u::predict_body(
        &output, previous, latest,
        1000.0, 1050.0, 1300.0, 20.0f);
    if (!near(output.JointData[vp4u::JT_HandLeft].PositionX, 0.15f)) {
        std::cerr << "prediction horizon was not capped at 100ms\n";
        return 1;
    }

    previous.JointData[vp4u::JT_HandLeft].TrackingState = 0;
    vp4u::predict_body(
        &output, previous, latest,
        1000.0, 1050.0, 1080.0, 20.0f);
    if (!near(output.JointData[vp4u::JT_HandLeft].PositionX, 0.05f)) {
        std::cerr << "an untracked measurement contaminated prediction\n";
        return 1;
    }

    previous.JointData[vp4u::JT_HandLeft].TrackingState = 2;
    previous.JointData[vp4u::JT_HandLeft].PositionX = 0.050f;
    latest.JointData[vp4u::JT_HandLeft].PositionX = 0.058f;
    vp4u::predict_body(
        &output, previous, latest,
        1000.0, 1050.0, 1100.0, 0.0f);
    if (!near(output.JointData[vp4u::JT_HandLeft].PositionX, 0.058f)) {
        std::cerr << "sub-jitter motion was amplified by prediction\n";
        return 1;
    }

    tracked(&previous, vp4u::JT_HandLeft, 0.0f);
    tracked(&latest, vp4u::JT_HandLeft, 1.0f);
    vp4u::predict_body(
        &output, previous, latest,
        1000.0, 1050.0, 1100.0, 0.0f);
    if (!near(output.JointData[vp4u::JT_HandLeft].PositionX, 1.25f)) {
        std::cerr << "extremity speed limit was not applied\n";
        return 1;
    }

    previous = {};
    latest = {};
    tracked(&previous, vp4u::JT_HandLeft, 0.0f);
    tracked(&previous, vp4u::JT_WristLeft, -0.10f);
    tracked(&previous, vp4u::JT_HandTipLeft, 0.10f);
    tracked(&previous, vp4u::JT_ThumbLeft, 0.02f);
    tracked(&latest, vp4u::JT_HandLeft, 0.10f);
    tracked(&latest, vp4u::JT_WristLeft, -0.02f);
    tracked(&latest, vp4u::JT_HandTipLeft, 0.23f);
    tracked(&latest, vp4u::JT_ThumbLeft, 0.15f);
    vp4u::predict_body(
        &output, previous, latest,
        1000.0, 1050.0, 1100.0, 0.0f);
    const float hand_delta =
        output.JointData[vp4u::JT_HandLeft].PositionX - 0.10f;
    for (const int joint : {vp4u::JT_WristLeft, vp4u::JT_HandTipLeft,
                            vp4u::JT_ThumbLeft}) {
        const float latest_x = latest.JointData[joint].PositionX;
        if (!near(output.JointData[joint].PositionX - latest_x,
                  hand_delta)) {
            std::cerr << "derived hand points did not move as a rigid group\n";
            return 1;
        }
    }
    if (!near(output.JointData[vp4u::JT_HandTipLeft].PositionX -
                  output.JointData[vp4u::JT_WristLeft].PositionX,
              latest.JointData[vp4u::JT_HandTipLeft].PositionX -
                  latest.JointData[vp4u::JT_WristLeft].PositionX)) {
        std::cerr << "hand extrapolation changed the measured IK direction\n";
        return 1;
    }

    tracked(&previous, vp4u::JT_AnkleLeft, 0.0f);
    tracked(&previous, vp4u::JT_FootLeft, 0.20f);
    tracked(&latest, vp4u::JT_AnkleLeft, 0.05f);
    tracked(&latest, vp4u::JT_FootLeft, 0.30f);
    vp4u::predict_body(
        &output, previous, latest,
        1000.0, 1050.0, 1100.0, 0.0f);
    if (!near(output.JointData[vp4u::JT_FootLeft].PositionX -
                  output.JointData[vp4u::JT_AnkleLeft].PositionX,
              0.25f)) {
        std::cerr << "foot extrapolation changed the measured IK direction\n";
        return 1;
    }

    std::cout << "Bounded body prediction passed\n";
    return 0;
}
