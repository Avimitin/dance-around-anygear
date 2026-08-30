#include "body_prediction.h"

#include <algorithm>
#include <cmath>

namespace vp4u {
namespace {

float prediction_speed_limit(int joint) {
    switch (joint) {
    case JT_ElbowLeft:
    case JT_WristLeft:
    case JT_HandLeft:
    case JT_ElbowRight:
    case JT_WristRight:
    case JT_HandRight:
    case JT_KneeLeft:
    case JT_AnkleLeft:
    case JT_FootLeft:
    case JT_KneeRight:
    case JT_AnkleRight:
    case JT_FootRight:
    case JT_HandTipLeft:
    case JT_ThumbLeft:
    case JT_HandTipRight:
    case JT_ThumbRight:
        return 5.0f;
    default:
        return 3.0f;
    }
}

float prediction_jitter_radius(int joint) {
    switch (joint) {
    case JT_WristLeft:
    case JT_HandLeft:
    case JT_WristRight:
    case JT_HandRight:
    case JT_AnkleLeft:
    case JT_FootLeft:
    case JT_AnkleRight:
    case JT_FootRight:
    case JT_HandTipLeft:
    case JT_ThumbLeft:
    case JT_HandTipRight:
    case JT_ThumbRight:
        return 0.012f;
    default:
        return 0.008f;
    }
}

int prediction_anchor_joint(int joint) {
    // VisionPose consumes wrist->hand/thumb and ankle->foot as orientation
    // vectors. The SPiKE model has one endpoint for each hand and foot, so
    // the remaining points are synthesized from that endpoint. Extrapolating
    // every synthesized point independently turns tiny orientation noise into
    // a large IK rotation. Translate each derived endpoint as one rigid group
    // and preserve the latest measured orientation instead.
    switch (joint) {
    case JT_WristLeft:
    case JT_HandLeft:
    case JT_HandTipLeft:
    case JT_ThumbLeft:
        return JT_HandLeft;
    case JT_WristRight:
    case JT_HandRight:
    case JT_HandTipRight:
    case JT_ThumbRight:
        return JT_HandRight;
    case JT_AnkleLeft:
    case JT_FootLeft:
        return JT_AnkleLeft;
    case JT_AnkleRight:
    case JT_FootRight:
        return JT_AnkleRight;
    case JT_Head:
    case JT_Nose:
    case JT_EyeLeft:
    case JT_EyeRight:
    case JT_EarLeft:
    case JT_EarRight:
        return JT_Head;
    default:
        return joint;
    }
}

} // namespace

void predict_body(ParsedBody* output,
                  const ParsedBody& previous,
                  const ParsedBody& latest,
                  double previous_sample_ms,
                  double latest_sample_ms,
                  double callback_time_ms,
                  float additional_prediction_ms,
                  float maximum_horizon_ms) {
    if (!output) return;
    *output = latest;

    const double sample_ms = latest_sample_ms - previous_sample_ms;
    if (!std::isfinite(sample_ms) || sample_ms < 15.0 || sample_ms > 250.0) {
        return;
    }
    const double age_ms = std::max(0.0, callback_time_ms - latest_sample_ms);
    const double horizon_ms = std::clamp(
        age_ms + std::max(0.0f, additional_prediction_ms),
        0.0,
        static_cast<double>(std::max(0.0f, maximum_horizon_ms)));
    if (horizon_ms <= 0.0) return;

    const float sample_seconds = static_cast<float>(sample_ms / 1000.0);
    const float horizon_seconds = static_cast<float>(horizon_ms / 1000.0);
    for (int joint = 0; joint < kJointCount; ++joint) {
        const int anchor = prediction_anchor_joint(joint);
        const Joint& old_joint = previous.JointData[anchor];
        const Joint& new_joint = latest.JointData[anchor];
        Joint& predicted = output->JointData[joint];
        if (previous.JointData[joint].TrackingState == 0 ||
            predicted.TrackingState == 0 || old_joint.TrackingState == 0 ||
            new_joint.TrackingState == 0) {
            continue;
        }
        const float displacement[3] = {
            new_joint.PositionX - old_joint.PositionX,
            new_joint.PositionY - old_joint.PositionY,
            new_joint.PositionZ - old_joint.PositionZ,
        };
        const float displacement_magnitude = std::sqrt(
            displacement[0] * displacement[0] +
            displacement[1] * displacement[1] +
            displacement[2] * displacement[2]);
        const float jitter_radius = prediction_jitter_radius(anchor);
        if (displacement_magnitude <= jitter_radius) {
            continue;
        }
        // Do not amplify frame-to-frame model noise while standing still.
        // Between one and two jitter radii, fade prediction in continuously;
        // measured positions still pass through immediately, so this creates
        // no dead zone in the actual pose.
        const float prediction_weight = std::min(
            1.0f,
            (displacement_magnitude - jitter_radius) / jitter_radius);
        float velocity[3] = {
            displacement[0] / sample_seconds * prediction_weight,
            displacement[1] / sample_seconds * prediction_weight,
            displacement[2] / sample_seconds * prediction_weight,
        };
        const float magnitude = displacement_magnitude / sample_seconds *
            prediction_weight;
        const float limit = prediction_speed_limit(anchor);
        if (magnitude > limit && magnitude > 0.0f) {
            const float scale = limit / magnitude;
            velocity[0] *= scale;
            velocity[1] *= scale;
            velocity[2] *= scale;
        }
        predicted.PositionX += velocity[0] * horizon_seconds;
        predicted.PositionY += velocity[1] * horizon_seconds;
        predicted.PositionZ += velocity[2] * horizon_seconds;
    }
}

} // namespace vp4u
